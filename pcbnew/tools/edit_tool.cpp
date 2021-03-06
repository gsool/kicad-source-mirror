/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2013-2017 CERN
 * @author Maciej Suminski <maciej.suminski@cern.ch>
 * @author Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 * Copyright (C) 2017-2020 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#include <limits>
#include <class_board.h>
#include <class_module.h>
#include <class_edge_mod.h>
#include <collectors.h>
#include <pcb_edit_frame.h>
#include <ws_proxy_view_item.h>
#include <kiway.h>
#include <array_creator.h>
#include <pcbnew_settings.h>
#include <status_popup.h>
#include <tool/selection_conditions.h>
#include <tool/tool_manager.h>
#include <tools/pcb_actions.h>
#include <tools/selection_tool.h>
#include <tools/edit_tool.h>
#include <tools/pcbnew_picker_tool.h>
#include <tools/tool_event_utils.h>
#include <tools/grid_helper.h>
#include <tools/pad_tool.h>
#include <pad_naming.h>
#include <view/view_controls.h>
#include <connectivity/connectivity_data.h>
#include <confirm.h>
#include <bitmaps.h>
#include <cassert>
#include <functional>
using namespace std::placeholders;
#include "kicad_clipboard.h"
#include <router/router_tool.h>
#include <dialogs/dialog_move_exact.h>
#include <dialogs/dialog_track_via_properties.h>
#include <board_commit.h>
#include <zone_filler.h>


void EditToolSelectionFilter( GENERAL_COLLECTOR& aCollector, int aFlags, SELECTION_TOOL* selectionTool )
{
    // Iterate from the back so we don't have to worry about removals.
    for( int i = aCollector.GetCount() - 1; i >= 0; --i )
    {
        BOARD_ITEM* item = aCollector[ i ];

        if( ( aFlags & EXCLUDE_LOCKED ) && item->IsLocked() )
        {
            aCollector.Remove( item );
        }
        else if( item->Type() == PCB_MODULE_ZONE_AREA_T )
        {
            MODULE* mod = static_cast<MODULE*>( item->GetParent() );

            // case 1: handle locking
            if( ( aFlags & EXCLUDE_LOCKED ) && mod && mod->IsLocked() )
            {
                aCollector.Remove( item );
            }

            // case 2: selection contains both the module and its pads - remove the pads
            if( !( aFlags & INCLUDE_PADS_AND_MODULES ) && mod && aCollector.HasItem( mod ) )
                aCollector.Remove( item );
        }
        else if( item->Type() == PCB_PAD_T )
        {
            MODULE* mod = static_cast<MODULE*>( item->GetParent() );

            // case 1: handle locking
            if( ( aFlags & EXCLUDE_LOCKED ) && mod && mod->IsLocked() )
            {
                aCollector.Remove( item );
            }
            else if( ( aFlags & EXCLUDE_LOCKED_PADS ) && mod && mod->PadsLocked() )
            {
                // Pad locking is considerably "softer" than item locking
                aCollector.Remove( item );

                if( !mod->IsLocked() && !aCollector.HasItem( mod ) )
                    aCollector.Append( mod );
            }

            // case 2: selection contains both the module and its pads - remove the pads
            if( !( aFlags & INCLUDE_PADS_AND_MODULES ) && mod && aCollector.HasItem( mod ) )
                aCollector.Remove( item );
        }
        else if( ( aFlags & EXCLUDE_TRANSIENTS ) && item->Type() == PCB_MARKER_T )
        {
            aCollector.Remove( item );
        }
    }
    selectionTool->FilterCollectorForGroups( aCollector );
}


EDIT_TOOL::EDIT_TOOL() :
    PCB_TOOL_BASE( "pcbnew.InteractiveEdit" ),
    m_selectionTool( NULL ),
    m_dragging( false ),
    m_lockedSelected( false )
{
}


void EDIT_TOOL::Reset( RESET_REASON aReason )
{
    m_dragging = false;

    m_statusPopup = std::make_unique<STATUS_TEXT_POPUP>( getEditFrame<PCB_BASE_EDIT_FRAME>() );

    if( aReason != RUN )
        m_commit.reset( new BOARD_COMMIT( this ) );
}


SPECIAL_TOOLS_CONTEXT_MENU::SPECIAL_TOOLS_CONTEXT_MENU( TOOL_INTERACTIVE* aTool ) :
        CONDITIONAL_MENU( aTool )
{
    SetIcon( options_board_xpm );
    SetTitle( _( "Special Tools..." ) );

    AddItem( PCB_ACTIONS::moveExact, SELECTION_CONDITIONS::ShowAlways );
    AddItem( PCB_ACTIONS::moveWithReference, SELECTION_CONDITIONS::ShowAlways );
    AddItem( PCB_ACTIONS::positionRelative, SELECTION_CONDITIONS::ShowAlways );
    AddItem( PCB_ACTIONS::createArray, SELECTION_CONDITIONS::ShowAlways );
}


bool EDIT_TOOL::Init()
{
    // Find the selection tool, so they can cooperate
    m_selectionTool = static_cast<SELECTION_TOOL*>( m_toolMgr->FindTool( "pcbnew.InteractiveSelection" ) );
    wxASSERT_MSG( m_selectionTool, "pcbnew.InteractiveSelection tool is not available" );

    auto editingModuleCondition =
            [ this ] ( const SELECTION& aSelection )
            {
                return m_editModules;
            };

    auto singleModuleCondition = SELECTION_CONDITIONS::OnlyType( PCB_MODULE_T )
                                    && SELECTION_CONDITIONS::Count( 1 );

    auto noActiveToolCondition =
            [ this ] ( const SELECTION& aSelection )
            {
                return frame()->ToolStackIsEmpty();
            };

    auto notMovingCondition =
            [ this ] ( const SELECTION& aSelection )
            {
                return !frame()->IsCurrentTool( PCB_ACTIONS::move )
                       && !frame()->IsCurrentTool( PCB_ACTIONS::moveWithReference );
            };

    auto noItemsCondition =
            [ this ] ( const SELECTION& aSelections ) -> bool
            {
                return frame()->GetBoard() && !frame()->GetBoard()->IsEmpty();
            };

    // Add context menu entries that are displayed when selection tool is active
    CONDITIONAL_MENU& menu = m_selectionTool->GetToolMenu().GetMenu();

    menu.AddItem( PCB_ACTIONS::move, SELECTION_CONDITIONS::NotEmpty && notMovingCondition );
    menu.AddItem( PCB_ACTIONS::inlineBreakTrack, SELECTION_CONDITIONS::Count( 1 )
                      && SELECTION_CONDITIONS::OnlyTypes( GENERAL_COLLECTOR::Tracks ) );
    menu.AddItem( PCB_ACTIONS::drag45Degree, SELECTION_CONDITIONS::OnlyTypes( GENERAL_COLLECTOR::Tracks ) );
    menu.AddItem( PCB_ACTIONS::dragFreeAngle, SELECTION_CONDITIONS::OnlyTypes( GENERAL_COLLECTOR::Tracks ) );
    menu.AddItem( PCB_ACTIONS::rotateCcw, SELECTION_CONDITIONS::NotEmpty );
    menu.AddItem( PCB_ACTIONS::rotateCw, SELECTION_CONDITIONS::NotEmpty );
    menu.AddItem( PCB_ACTIONS::flip, SELECTION_CONDITIONS::NotEmpty );
    menu.AddItem( PCB_ACTIONS::mirror, editingModuleCondition && SELECTION_CONDITIONS::NotEmpty );

    menu.AddItem( ACTIONS::doDelete, SELECTION_CONDITIONS::NotEmpty );
    menu.AddItem( PCB_ACTIONS::properties, SELECTION_CONDITIONS::Count( 1 )
                      || SELECTION_CONDITIONS::OnlyTypes( GENERAL_COLLECTOR::Tracks ) );

    menu.AddItem( ACTIONS::duplicate, SELECTION_CONDITIONS::NotEmpty );

    // Add the submenu for create array and special move
    auto specialToolsSubMenu = std::make_shared<SPECIAL_TOOLS_CONTEXT_MENU>( this );
    menu.AddSeparator();
    m_selectionTool->GetToolMenu().AddSubMenu( specialToolsSubMenu );
    menu.AddMenu( specialToolsSubMenu.get(), SELECTION_CONDITIONS::NotEmpty, 100 );

    menu.AddSeparator( 150 );
    menu.AddItem( ACTIONS::cut, SELECTION_CONDITIONS::NotEmpty, 150 );
    menu.AddItem( ACTIONS::copy, SELECTION_CONDITIONS::NotEmpty, 150 );
    // Selection tool handles the context menu for some other tools, such as the Picker.
    // Don't add things like Paste when another tool is active.
    menu.AddItem( ACTIONS::paste, noActiveToolCondition, 150 );
    menu.AddItem( ACTIONS::selectAll, noItemsCondition, 150 );

    // Footprint actions
    menu.AddSeparator( 150 );
    menu.AddItem( PCB_ACTIONS::editFpInFpEditor, singleModuleCondition, 150 );
    menu.AddItem( PCB_ACTIONS::updateFootprint, singleModuleCondition, 150 );
    menu.AddItem( PCB_ACTIONS::changeFootprint, singleModuleCondition, 150 );

    return true;
}


int EDIT_TOOL::GetAndPlace( const TOOL_EVENT& aEvent )
{
    SELECTION_TOOL* selectionTool = m_toolMgr->GetTool<SELECTION_TOOL>();
    MODULE*         module = getEditFrame<PCB_BASE_FRAME>()->GetFootprintFromBoardByReference();

    if( module )
    {
        m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );
        m_toolMgr->RunAction( PCB_ACTIONS::selectItem, true, (void*) module );

        selectionTool->GetSelection().SetReferencePoint( module->GetPosition() );
        m_toolMgr->RunAction( PCB_ACTIONS::move, false );
    }

    return 0;
}


bool EDIT_TOOL::invokeInlineRouter( int aDragMode )
{
    ROUTER_TOOL* theRouter = m_toolMgr->GetTool<ROUTER_TOOL>();

    if( !theRouter )
        return false;

    // don't allow switch from moving to dragging
    if( m_dragging )
    {
        wxBell();
        return false;
    }

	// make sure we don't accidentally invoke inline routing mode while the router is already active!
    if( theRouter->IsToolActive() )
        return false;

    if( theRouter->CanInlineDrag() )
    {
        m_toolMgr->RunAction( PCB_ACTIONS::routerInlineDrag, true, aDragMode );
        return true;
    }

    return false;
}


bool EDIT_TOOL::isInteractiveDragEnabled() const
{
    ROUTER_TOOL* router = m_toolMgr->GetTool<ROUTER_TOOL>();

    return router && router->Router()->Settings().InlineDragEnabled();
}


bool EDIT_TOOL::isRouterActive() const
{
    ROUTER_TOOL* router = m_toolMgr->GetTool<ROUTER_TOOL>();

    return router && router->IsToolActive();
}


int EDIT_TOOL::Drag( const TOOL_EVENT& aEvent )
{
    int mode = PNS::DM_ANY;

    if( aEvent.IsAction( &PCB_ACTIONS::dragFreeAngle ) )
        mode |= PNS::DM_FREE_ANGLE;

    invokeInlineRouter( mode );

    return 0;
}


int EDIT_TOOL::Move( const TOOL_EVENT& aEvent )
{
    if( isRouterActive() )
    {
        wxBell();
        return 0;
    }

    return doMoveSelection( aEvent );
}


int EDIT_TOOL::MoveWithReference( const TOOL_EVENT& aEvent )
{
    if( isRouterActive() )
    {
        wxBell();
        return 0;
    }

    return doMoveSelection( aEvent, true );
}


// Note: aEvent MUST NOT be const&; the source will get de-allocated if we go into the picker's
// event loop.
int EDIT_TOOL::doMoveSelection( TOOL_EVENT aEvent, bool aPickReference )
{
    PCB_BASE_EDIT_FRAME*  editFrame = getEditFrame<PCB_BASE_EDIT_FRAME>();
    KIGFX::VIEW_CONTROLS* controls  = getViewControls();
    VECTOR2I              originalCursorPos = controls->GetCursorPosition();

    // Be sure that there is at least one item that we can modify. If nothing was selected before,
    // try looking for the stuff under mouse cursor (i.e. Kicad old-style hover selection)
    PCBNEW_SELECTION& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_TRANSIENTS, sTool );
            } );

    if( m_dragging || selection.Empty() )
        return 0;

    LSET     item_layers = selection.GetSelectionLayers();
    bool     unselect    = selection.IsHover(); // N.B. This must be saved before the re-selection below
    VECTOR2I pickedReferencePoint;

    // Now filter out locked pads.  We cannot do this in the first RequestSelection() as we need
    // the item_layers when a pad is the selection front (ie: will become curr_tiem).
    selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_LOCKED_PADS, sTool );
            } );

    if( selection.Empty() )
        return 0;

    std::string tool = aEvent.GetCommandStr().get();
    editFrame->PushTool( tool );
    Activate();
    controls->ShowCursor( true );
    controls->SetAutoPan( true );

    if( aPickReference && !pickReferencePoint( _( "Select reference point for move..." ), "", "",
                                               pickedReferencePoint ) )
    {
        if( unselect )
            m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

        editFrame->PopTool( tool );
        return 0;
    }

    std::vector<BOARD_ITEM*> sel_items;

    for( EDA_ITEM* item : selection )
    {
        BOARD_ITEM* boardItem = dynamic_cast<BOARD_ITEM*>( item );
        MODULE*     module    = dynamic_cast<MODULE*>( item );

        if( boardItem )
            sel_items.push_back( boardItem );

        if( module )
        {
            for( D_PAD* pad : module->Pads() )
                sel_items.push_back( pad );
        }
    }

    bool        restore_state = false;
    bool        fromOtherCommand = false;
    VECTOR2I    totalMovement;
    GRID_HELPER grid( m_toolMgr, editFrame->GetMagneticItemsSettings() );
    TOOL_EVENT* evt = const_cast<TOOL_EVENT*>( &aEvent );
    VECTOR2I    prevPos;

    // Prime the pump
    m_toolMgr->RunAction( ACTIONS::refreshPreview );

    // Main loop: keep receiving events
    do
    {
        VECTOR2I movement;
        editFrame->GetCanvas()->SetCurrentCursor( wxCURSOR_ARROW );
        grid.SetSnap( !evt->Modifier( MD_SHIFT ) );
        grid.SetUseGrid( !evt->Modifier( MD_ALT ) );

        if( evt->IsAction( &PCB_ACTIONS::move ) || evt->IsMotion() || evt->IsDrag( BUT_LEFT )
                || evt->IsAction( &ACTIONS::refreshPreview )
                || evt->IsAction( &PCB_ACTIONS::moveWithReference ) )
        {
            if( m_dragging && evt->Category() == TC_MOUSE )
            {
                VECTOR2I mousePos( controls->GetMousePosition() );

                m_cursor = grid.BestSnapAnchor( mousePos, item_layers, sel_items );

                if( controls->GetSettings().m_lastKeyboardCursorPositionValid )
                {
                    long action = controls->GetSettings().m_lastKeyboardCursorCommand;

                    // The arrow keys are by definition SINGLE AXIS.  Do not allow the other
                    // axis to be snapped to the grid.
                    if( action == ACTIONS::CURSOR_LEFT || action == ACTIONS::CURSOR_RIGHT )
                        m_cursor.y = prevPos.y;
                    else if( action == ACTIONS::CURSOR_UP || action == ACTIONS::CURSOR_DOWN )
                        m_cursor.x = prevPos.x;
                }

                controls->ForceCursorPosition( true, m_cursor );
                selection.SetReferencePoint( m_cursor );

                movement = m_cursor - prevPos;
                prevPos = m_cursor;
                totalMovement += movement;

                // Drag items to the current cursor position
                for( EDA_ITEM* item : sel_items )
                {
                    // Don't double move footprint pads, fields, etc.
                    //
                    // For PCB_GROUP_T, we make sure the selection includes only the top level
                    // group and not its descendants.
                    if( !item->GetParent() || !item->GetParent()->IsSelected() )
                        static_cast<BOARD_ITEM*>( item )->Move( movement );
                }

                m_toolMgr->PostEvent( EVENTS::SelectedItemsMoved );
            }
            else if( !m_dragging && !evt->IsAction( &ACTIONS::refreshPreview ) )
            {
                // Prepare to start dragging

                if( !( evt->IsAction( &PCB_ACTIONS::move )
                       || evt->IsAction( &PCB_ACTIONS::moveWithReference ) )
                    && isInteractiveDragEnabled() )
                {
                    if( invokeInlineRouter( PNS::DM_ANY ) )
                        break;
                }

                // deal with locked items (override lock or abort the operation)
                SELECTION_LOCK_FLAGS lockFlags = m_selectionTool->CheckLock();

                if( lockFlags == SELECTION_LOCKED )
                    break;

                m_dragging = true;

                // When editing modules, all items have the same parent
                if( EditingModules() )
                {
                    m_commit->Modify( selection.Front() );
                }
                else
                {
                    // Save items, so changes can be undone
                    for( EDA_ITEM* item : selection )
                    {
                        // Don't double move footprint pads, fields, etc.
                        //
                        // For PCB_GROUP_T, the parent is the board.
                        if( item->GetParent() && item->GetParent()->IsSelected() )
                            continue;

                        m_commit->Modify( item );

                        // If moving a group, record position of all the descendants for undo
                        if( item->Type() == PCB_GROUP_T )
                        {
                            PCB_GROUP* group = static_cast<PCB_GROUP*>( item );
                            group->RunOnDescendants( [&]( BOARD_ITEM* bItem )
                                                     {
                                                         m_commit->Modify( bItem );
                                                     });
                        }

                        if( item->IsNew() )
                            fromOtherCommand = true;
                    }
                }

                editFrame->UndoRedoBlock( true );
                m_cursor = controls->GetCursorPosition();

                if( selection.HasReferencePoint() )
                {
                    // start moving with the reference point attached to the cursor
                    grid.SetAuxAxes( false );

                    movement = m_cursor - selection.GetReferencePoint();

                    // Drag items to the current cursor position
                    for( EDA_ITEM* item : selection )
                    {
                        // Don't double move footprint pads, fields, etc.
                        if( item->GetParent() && item->GetParent()->IsSelected() )
                            continue;

                        static_cast<BOARD_ITEM*>( item )->Move( movement );
                    }

                    selection.SetReferencePoint( m_cursor );
                }
                else
                {
                    std::vector<BOARD_ITEM*> items;

                    for( EDA_ITEM* item : selection )
                        items.push_back( static_cast<BOARD_ITEM*>( item ) );

                    m_cursor = grid.BestDragOrigin( originalCursorPos, items );

                    // Set the current cursor position to the first dragged item origin, so the
                    // movement vector could be computed later
                    if( aPickReference )
                    {
                        selection.SetReferencePoint( pickedReferencePoint );
                        controls->ForceCursorPosition( true, pickedReferencePoint );
                        m_cursor = pickedReferencePoint;
                    }
                    else
                    {
                        // Check if user wants to warp the mouse to origin of moved object
                        if( !editFrame->GetMoveWarpsCursor() )
                            m_cursor = originalCursorPos; // No, so use original mouse pos instead

                        selection.SetReferencePoint( m_cursor );
                        grid.SetAuxAxes( true, m_cursor );
                    }
                }

                controls->SetCursorPosition( m_cursor, false );

                prevPos = m_cursor;
                controls->SetAutoPan( true );
                m_toolMgr->PostEvent( EVENTS::SelectedItemsModified );
            }

            m_toolMgr->RunAction( PCB_ACTIONS::updateLocalRatsnest, false, new VECTOR2I( movement ) );
        }

        else if( evt->IsCancelInteractive() || evt->IsActivate() )
        {
            if( m_dragging && evt->IsCancelInteractive() )
                evt->SetPassEvent( false );

            restore_state = true; // Canceling the tool means that items have to be restored
            break;                // Finish
        }

        else if( evt->IsAction( &ACTIONS::undo ) )
        {
            restore_state = true; // Perform undo locally
            break;                // Finish
        }

        // Dispatch TOOL_ACTIONs
        else if( evt->IsAction( &ACTIONS::doDelete ) )
        {
            break; // finish -- there is no further processing for removed items
        }
        else if( evt->IsAction( &ACTIONS::duplicate ) )
        {
            break; // finish -- Duplicate tool will start a new Move with the dup'ed items
        }
        else if( evt->IsAction( &PCB_ACTIONS::moveExact ) )
        {
            // Reset positions so the Move Exactly is from the start.
            for( EDA_ITEM* item : selection )
            {
                BOARD_ITEM* i = static_cast<BOARD_ITEM*>( item );
                i->Move( -totalMovement );
            }

            break; // finish -- we moved exactly, so we are finished
        }

        else if( evt->IsMouseUp( BUT_LEFT ) || evt->IsClick( BUT_LEFT ) )
        {
            break; // finish
        }

        else
        {
            evt->SetPassEvent();
        }

    } while( ( evt = Wait() ) ); // Assignment (instead of equality test) is intentional

    m_lockedSelected = false;
    controls->ForceCursorPosition( false );
    controls->ShowCursor( false );
    controls->SetAutoPan( false );

    m_dragging = false;
    editFrame->UndoRedoBlock( false );

    // Discard reference point when selection is "dropped" onto the board (ie: not dragging anymore)
    selection.ClearReferencePoint();

    // If canceled, we need to remove the dynamic ratsnest from the screen
    if( restore_state )
    {
        m_commit->Revert();

        if( fromOtherCommand )
        {
            PICKED_ITEMS_LIST* undo = editFrame->PopCommandFromUndoList();

            if( undo )
            {
                editFrame->PutDataInPreviousState( undo, false );
                undo->ClearListAndDeleteItems();
                delete undo;
            }
        }
    }
    else
    {
        m_commit->Push( _( "Drag" ) );
    }

    m_toolMgr->RunAction( PCB_ACTIONS::hideDynamicRatsnest, true );

    if( unselect )
        m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

    editFrame->PopTool( tool );
    return 0;
}


int EDIT_TOOL::ChangeTrackWidth( const TOOL_EVENT& aEvent )
{
    const auto& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_TRANSIENTS, sTool );
            } );

    for( EDA_ITEM* item : selection )
    {
        if( auto via = dyn_cast<VIA*>( item ) )
        {
            m_commit->Modify( item );

            int new_width;
            int new_drill;

            if( via->GetViaType() == VIATYPE::MICROVIA )
            {
                NETINFO_ITEM* net = via->GetNet();

                new_width = net->GetMicroViaSize();
                new_drill = net->GetMicroViaDrillSize();
            }
            else
            {
                new_width = board()->GetDesignSettings().GetCurrentViaSize();
                new_drill = board()->GetDesignSettings().GetCurrentViaDrill();
            }

            via->SetDrill( new_drill );
            via->SetWidth( new_width );
        }
        else if ( TRACK* track = dyn_cast<TRACK*>( item ) )
        {
            m_commit->Modify( item );

            int new_width = board()->GetDesignSettings().GetCurrentTrackWidth();
            track->SetWidth( new_width );
        }
    }

    m_commit->Push( _("Edit track width/via size") );

    if( selection.IsHover() )
    {
        m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

        // Notify other tools of the changes -- This updates the visual ratsnest
        m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );
    }

    return 0;
}


int EDIT_TOOL::Properties( const TOOL_EVENT& aEvent )
{
    PCB_BASE_EDIT_FRAME*    editFrame = getEditFrame<PCB_BASE_EDIT_FRAME>();
    const PCBNEW_SELECTION& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_TRANSIENTS, sTool );
            } );

    // Tracks & vias are treated in a special way:
    if( ( SELECTION_CONDITIONS::OnlyTypes( GENERAL_COLLECTOR::Tracks ) )( selection ) )
    {
            DIALOG_TRACK_VIA_PROPERTIES dlg( editFrame, selection, *m_commit );
            dlg.ShowQuasiModal();       // QuasiModal required for NET_SELECTOR
    }
    else if( selection.Size() == 1 )
    {
        // Display properties dialog
        BOARD_ITEM* item = static_cast<BOARD_ITEM*>( selection.Front() );

        // Do not handle undo buffer, it is done by the properties dialogs
        editFrame->OnEditItemRequest( item );

        // Notify other tools of the changes
        m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );
    }
    else if( selection.Size() == 0 && getView()->IsLayerVisible( LAYER_WORKSHEET ) )
    {
        KIGFX::WS_PROXY_VIEW_ITEM* worksheet = editFrame->GetCanvas()->GetWorksheet();
        VECTOR2D cursorPos = getViewControls()->GetCursorPosition( false );

        if( worksheet && worksheet->HitTestWorksheetItems( getView(), (wxPoint) cursorPos ) )
            m_toolMgr->RunAction( ACTIONS::pageSettings );
    }

    if( selection.IsHover() )
    {
        m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

        // Notify other tools of the changes -- This updates the visual ratsnest
        m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );
    }

    return 0;
}


int EDIT_TOOL::Rotate( const TOOL_EVENT& aEvent )
{
    if( isRouterActive() )
    {
        wxBell();
        return 0;
    }

    PCB_BASE_EDIT_FRAME* editFrame = getEditFrame<PCB_BASE_EDIT_FRAME>();

    auto& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_LOCKED_PADS | EXCLUDE_TRANSIENTS, sTool );
            },
            nullptr, ! m_dragging );

    if( selection.Empty() )
        return 0;

    updateModificationPoint( selection );
    auto refPt = selection.GetReferencePoint();
    const int rotateAngle = TOOL_EVT_UTILS::GetEventRotationAngle( *editFrame, aEvent );

    // When editing modules, all items have the same parent
    if( EditingModules() )
        m_commit->Modify( selection.Front() );

    for( auto item : selection )
    {
        if( !item->IsNew() && !EditingModules() )
        {
            m_commit->Modify( item );

            // If rotating a group, record position of all the descendants for undo
            if( item->Type() == PCB_GROUP_T )
            {
                static_cast<PCB_GROUP*>( item )->RunOnDescendants( [&]( BOARD_ITEM* bItem )
                                                                   {
                                                                       m_commit->Modify( bItem );
                                                                   });
            }
        }

        static_cast<BOARD_ITEM*>( item )->Rotate( refPt, rotateAngle );
    }

    if( !m_dragging )
        m_commit->Push( _( "Rotate" ) );

    if( selection.IsHover() && !m_dragging )
        m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

    m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );

    if( m_dragging )
        m_toolMgr->RunAction( PCB_ACTIONS::updateLocalRatsnest, false );

    return 0;
}


/*!
 * Mirror a point about the vertical axis passing through another point
 */
static wxPoint mirrorPointX( const wxPoint& aPoint, const wxPoint& aMirrorPoint )
{
    wxPoint mirrored = aPoint;

    mirrored.x -= aMirrorPoint.x;
    mirrored.x = -mirrored.x;
    mirrored.x += aMirrorPoint.x;

    return mirrored;
}


/**
 * Mirror a pad in the vertical axis passing through a point (mirror left to right)
 */
static void mirrorPadX( D_PAD& aPad, const wxPoint& aMirrorPoint )
{
    if( aPad.GetShape() == PAD_SHAPE_CUSTOM )
        aPad.FlipPrimitives( true );  // mirror primitives left to right

    wxPoint tmpPt = mirrorPointX( aPad.GetPosition(), aMirrorPoint );
    aPad.SetPosition( tmpPt );

    aPad.SetX0( aPad.GetPosition().x );

    tmpPt = aPad.GetOffset();
    tmpPt.x = -tmpPt.x;
    aPad.SetOffset( tmpPt );

    auto tmpz = aPad.GetDelta();
    tmpz.x = -tmpz.x;
    aPad.SetDelta( tmpz );

    aPad.SetOrientation( -aPad.GetOrientation() );
}


int EDIT_TOOL::Mirror( const TOOL_EVENT& aEvent )
{
    if( isRouterActive() )
    {
        wxBell();
        return 0;
    }

    auto& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_LOCKED_PADS | EXCLUDE_TRANSIENTS, sTool );
            },
            nullptr, !m_dragging );

    if( selection.Empty() )
        return 0;

    updateModificationPoint( selection );
    auto refPoint = selection.GetReferencePoint();
    wxPoint mirrorPoint( refPoint.x, refPoint.y );

    // When editing modules, all items have the same parent
    if( EditingModules() )
        m_commit->Modify( selection.Front() );

    for( EDA_ITEM* item : selection )
    {
        // only modify items we can mirror
        switch( item->Type() )
        {
        case PCB_MODULE_EDGE_T:
        case PCB_MODULE_TEXT_T:
        case PCB_MODULE_ZONE_AREA_T:
        case PCB_PAD_T:
            // Only create undo entry for items on the board
            if( !item->IsNew() && !EditingModules() )
                m_commit->Modify( item );

            break;
        default:
            continue;
        }

        // modify each object as necessary
        switch( item->Type() )
        {
        case PCB_MODULE_EDGE_T:
        {
            EDGE_MODULE* edge = static_cast<EDGE_MODULE*>( item );
            edge->Mirror( mirrorPoint, false );
            break;
        }

        case PCB_MODULE_ZONE_AREA_T:
        {
            MODULE_ZONE_CONTAINER* zone = static_cast<MODULE_ZONE_CONTAINER*>( item );
            zone->Mirror( mirrorPoint, false );
            break;
        }

        case PCB_MODULE_TEXT_T:
        {
            TEXTE_MODULE* modText = static_cast<TEXTE_MODULE*>( item );
            modText->Mirror( mirrorPoint, false );
            break;
        }

        case PCB_PAD_T:
        {
            D_PAD* pad = static_cast<D_PAD*>( item );
            mirrorPadX( *pad, mirrorPoint );
            break;
        }

        default:
            // it's likely the commit object is wrong if you get here
            // Unsure if PCB_GROUP_T needs special attention here.
            assert( false );
            break;
        }
    }

    if( !m_dragging )
        m_commit->Push( _( "Mirror" ) );

    if( selection.IsHover() && !m_dragging )
        m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

    m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );

    if( m_dragging )
        m_toolMgr->RunAction( PCB_ACTIONS::updateLocalRatsnest, false );

    return 0;
}


int EDIT_TOOL::Flip( const TOOL_EVENT& aEvent )
{
    if( isRouterActive() )
    {
        wxBell();
        return 0;
    }

    auto& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_LOCKED_PADS | EXCLUDE_TRANSIENTS, sTool );
            },
            nullptr, !m_dragging );

    if( selection.Empty() )
        return 0;

    updateModificationPoint( selection );

    // Flip around the anchor for footprints, and the bounding box center for board items
    VECTOR2I modPoint = EditingModules() ? VECTOR2I( 0, 0 ) : selection.GetCenter();

    // If only one item selected, flip around the item anchor point, instead
    // of the bounding box center, to avoid moving the item anchor
    if( selection.GetSize() == 1 )
        modPoint = static_cast<BOARD_ITEM*>( selection.GetItem( 0 ) )->GetPosition();

    bool leftRight = frame()->Settings().m_FlipLeftRight;

    // When editing modules, all items have the same parent
    if( EditingModules() )
        m_commit->Modify( selection.Front() );

    for( EDA_ITEM* item : selection )
    {
        if( !item->IsNew() && !EditingModules() )
            m_commit->Modify( item );

        if( item->Type() == PCB_GROUP_T )
        {
            static_cast<PCB_GROUP*>( item )->RunOnDescendants( [&]( BOARD_ITEM* bItem )
                                                               {
                                                                   m_commit->Modify( bItem );
                                                               });
        }

        static_cast<BOARD_ITEM*>( item )->Flip( modPoint, leftRight );
    }

    if( !m_dragging )
        m_commit->Push( _( "Flip" ) );

    if( selection.IsHover() && !m_dragging )
        m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

    m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );

    if( m_dragging )
        m_toolMgr->RunAction( PCB_ACTIONS::updateLocalRatsnest, false );

    return 0;
}


int EDIT_TOOL::Remove( const TOOL_EVENT& aEvent )
{
    if( isRouterActive() )
    {
        wxBell();
        return 0;
    }

    std::vector<BOARD_ITEM*> lockedItems;
    Activate();

    // get a copy instead of reference (as we're going to clear the selection before removing items)
    PCBNEW_SELECTION selectionCopy;
    bool isCut = aEvent.Parameter<PCB_ACTIONS::REMOVE_FLAGS>() == PCB_ACTIONS::REMOVE_FLAGS::CUT;
    bool isAlt = aEvent.Parameter<PCB_ACTIONS::REMOVE_FLAGS>() == PCB_ACTIONS::REMOVE_FLAGS::ALT;

    // If we are in a "Cut" operation, then the copied selection exists already
    if( isCut )
    {
        selectionCopy = m_selectionTool->GetSelection();
    }
    else
    {
        selectionCopy = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_LOCKED_PADS | EXCLUDE_TRANSIENTS, sTool );
            } );
    }

    bool isHover = selectionCopy.IsHover();

    // in "alternative" mode, deletion is not just a simple list of selected items,
    // it removes whole tracks, not just segments
    if( isAlt && isHover
            && ( selectionCopy.HasType( PCB_TRACE_T ) || selectionCopy.HasType( PCB_VIA_T ) ) )
    {
        m_toolMgr->RunAction( PCB_ACTIONS::selectConnection, true );
    }

    if( selectionCopy.Empty() )
        return 0;

    // N.B. Setting the CUT flag prevents lock filtering as we only want to delete the items that
    // were copied to the clipboard, no more, no fewer.  Filtering for locked item, if any will be done
    // in the copyToClipboard() routine
    if( !m_lockedSelected && !isCut )
    {
        // Second RequestSelection removes locked items but keeps a copy of their pointers
        selectionCopy = m_selectionTool->RequestSelection(
                []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
                {
                    EditToolSelectionFilter( aCollector, EXCLUDE_LOCKED, sTool );
                },
                &lockedItems );
    }


    // As we are about to remove items, they have to be removed from the selection first
    m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

    PCBNEW_SELECTION removed;

    for( EDA_ITEM* item : selectionCopy )
    {
        if( m_editModules )
        {
            m_commit->Remove( item );
            removed.Add( item );
            continue;
        }

        switch( item->Type() )
        {
        case PCB_MODULE_TEXT_T:
            {
                TEXTE_MODULE* text = static_cast<TEXTE_MODULE*>( item );
                MODULE*       parent = static_cast<MODULE*>( item->GetParent() );

                if( text->GetType() == TEXTE_MODULE::TEXT_is_DIVERS )
                {
                    m_commit->Modify( text );
                    getView()->Remove( text );
                    parent->Remove( text );
                }
            }
            break;

        case PCB_PAD_T:
            {
                D_PAD*  pad = static_cast<D_PAD*>( item );
                MODULE* parent = static_cast<MODULE*>( item->GetParent() );

                m_commit->Modify( parent );
                getView()->Remove( pad );
                parent->Remove( pad );
            }
            break;

        case PCB_MODULE_ZONE_AREA_T:
            {
                MODULE_ZONE_CONTAINER* zone = static_cast<MODULE_ZONE_CONTAINER*>( item );
                MODULE*                parent = static_cast<MODULE*>( item->GetParent() );

                m_commit->Modify( parent );
                getView()->Remove( zone );
                parent->Remove( zone );
            }
            break;

        case PCB_ZONE_AREA_T:
            // We process the zones special so that cutouts can be deleted when the delete tool
            // is called from inside a cutout when the zone is selected.
            {
                // Only interact with cutouts when deleting and a single item is selected
                if( !isCut && selectionCopy.GetSize() == 1 )
                {
                    VECTOR2I        curPos = getViewControls()->GetCursorPosition();
                    ZONE_CONTAINER* zone   = static_cast<ZONE_CONTAINER*>( item );

                    int outlineIdx, holeIdx;

                    if( zone->HitTestCutout( curPos, &outlineIdx, &holeIdx ) )
                    {
                        // Remove the cutout
                        m_commit->Modify( zone );
                        zone->RemoveCutout( outlineIdx, holeIdx );

                        std::vector<ZONE_CONTAINER*> toFill;
                        toFill.emplace_back( zone );

                        // Fill the modified zone
                        ZONE_FILLER  filler( board(), m_commit.get() );
                        filler.InstallNewProgressReporter( frame(), _( "Fill Zone" ), 4 );

                        if( !filler.Fill( toFill ) )
                        {
                            m_commit->Revert();
                            return 1;
                        }

                        // Update the display
                        zone->HatchBorder();
                        canvas()->Refresh();

                        // Restore the selection on the original zone
                        m_toolMgr->RunAction( PCB_ACTIONS::selectItem, true, zone );

                        break;
                    }
                }

                // Remove the entire zone otherwise
                m_commit->Remove( item );
                removed.Add( item );
            }
            break;

        case PCB_GROUP_T:
            {
                m_commit->Remove( item );
                removed.Add( item );

                static_cast<PCB_GROUP*>( item )->RunOnDescendants( [&]( BOARD_ITEM* bItem )
                                                                   {
                                                                       m_commit->Remove( bItem );
                                                                   });
            }
            break;

        default:
            m_commit->Remove( item );
            removed.Add( item );
            break;
        }
    }

    // Figure out status of a group containing items to be removed.  if entered
    // group is not set in the selection tool, then any groups to be removed are
    // removed in their entirety and so no empty group could remain.  If entered
    // group is set, then we could be removing all items of the entered group,
    // in which case we need to remove the group itself.
    PCB_GROUP* enteredGroup = m_selectionTool->GetEnteredGroup();

    if( enteredGroup != nullptr )
    {
        board()->GroupRemoveItems( removed, m_commit.get() );

        if( m_commit->HasRemoveEntry( enteredGroup ) )
            m_selectionTool->ExitGroup();
    }

    if( isCut )
        m_commit->Push( _( "Cut" ) );
    else
        m_commit->Push( _( "Delete" ) );

    if( enteredGroup != nullptr )
    {
        wxString check = board()->GroupsSanityCheck();
        wxCHECK_MSG( check == wxEmptyString, 0,
                     _( "Remove of items in entered group resulted in inconsistent state: " )+ check );
    }

    if( !m_lockedSelected && !lockedItems.empty() )
    {
        ///> Popup nag for deleting locked items
        m_lockedSelected = true;
        m_toolMgr->RunAction( PCB_ACTIONS::selectItems, true, &lockedItems );
        m_statusPopup->SetText( _( "Delete again to remove locked items" ) );
        m_statusPopup->PopupFor( 2000 );
        m_statusPopup->Move( wxGetMousePosition() + wxPoint( 20, 20 ) );

        Activate();

        while( m_lockedSelected && m_statusPopup->IsShown() )
        {
            m_statusPopup->Move( wxGetMousePosition() + wxPoint( 20, 20 ) );
            Wait();
        }

        // Ensure statusPopup is hidden after use
        m_statusPopup->Hide();
    }

    m_lockedSelected = false;

    return 0;
}


int EDIT_TOOL::MoveExact( const TOOL_EVENT& aEvent )
{
    if( isRouterActive() )
    {
        wxBell();
        return 0;
    }

    const auto& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector,
                                      EXCLUDE_LOCKED | EXCLUDE_LOCKED_PADS | EXCLUDE_TRANSIENTS, sTool );
            } );

    if( selection.Empty() )
        return 0;

    wxPoint         translation;
    double          rotation;
    ROTATION_ANCHOR rotationAnchor = selection.Size() > 1 ? ROTATE_AROUND_SEL_CENTER
                                                          : ROTATE_AROUND_ITEM_ANCHOR;

    // TODO: Implement a visible bounding border at the edge
    auto sel_box = selection.GetBoundingBox();

    DIALOG_MOVE_EXACT dialog( frame(), translation, rotation, rotationAnchor, sel_box );
    int ret = dialog.ShowModal();

    if( ret == wxID_OK )
    {
        VECTOR2I rp = selection.GetCenter();
        wxPoint selCenter( rp.x, rp.y );

        // Make sure the rotation is from the right reference point
        selCenter += translation;

        // When editing modules, all items have the same parent
        if( EditingModules() )
            m_commit->Modify( selection.Front() );

        for( EDA_ITEM* selItem : selection )
        {
            BOARD_ITEM* item = static_cast<BOARD_ITEM*>( selItem );

            if( !item->IsNew() && !EditingModules() )
            {
                m_commit->Modify( item );

                if( item->Type() == PCB_GROUP_T )
                    {
                        static_cast<PCB_GROUP*>( item )->RunOnDescendants(
                                [&]( BOARD_ITEM* bItem )
                                {
                                    m_commit->Modify( bItem );
                                });
                    }
            }

            item->Move( translation );

            switch( rotationAnchor )
            {
            case ROTATE_AROUND_ITEM_ANCHOR:
                item->Rotate( item->GetPosition(), rotation );
                break;
            case ROTATE_AROUND_SEL_CENTER:
                item->Rotate( selCenter, rotation );
                break;
            case ROTATE_AROUND_USER_ORIGIN:
                item->Rotate( (wxPoint) frame()->GetScreen()->m_LocalOrigin, rotation );
                break;
            case ROTATE_AROUND_AUX_ORIGIN:
                item->Rotate( board()->GetDesignSettings().m_AuxOrigin, rotation );
                break;
            }

            if( !m_dragging )
                getView()->Update( item );
        }

        m_commit->Push( _( "Move exact" ) );

        if( selection.IsHover() )
            m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

        m_toolMgr->ProcessEvent( EVENTS::SelectedItemsModified );

        if( m_dragging )
            m_toolMgr->RunAction( PCB_ACTIONS::updateLocalRatsnest, false );
    }

    return 0;
}


int EDIT_TOOL::Duplicate( const TOOL_EVENT& aEvent )
{
    if( isRouterActive() )
    {
        wxBell();
        return 0;
    }

    bool increment = aEvent.IsAction( &PCB_ACTIONS::duplicateIncrement );

    // Be sure that there is at least one item that we can modify
    const auto& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_LOCKED_PADS | EXCLUDE_TRANSIENTS, sTool );
            } );

    if( selection.Empty() )
        return 0;

    // we have a selection to work on now, so start the tool process
    PCB_BASE_EDIT_FRAME* editFrame = getEditFrame<PCB_BASE_EDIT_FRAME>();

    // If the selection was given a hover, we do not keep the selection after completion
    bool is_hover = selection.IsHover();

    std::vector<BOARD_ITEM*> new_items;
    new_items.reserve( selection.Size() );


    // Each selected item is duplicated and pushed to new_items list
    // Old selection is cleared, and new items are then selected.
    for( EDA_ITEM* item : selection )
    {
        BOARD_ITEM* dupe_item = nullptr;
        BOARD_ITEM* orig_item = static_cast<BOARD_ITEM*>( item );

        if( m_editModules )
        {
            MODULE* editModule = editFrame->GetBoard()->GetFirstModule();
            dupe_item = editModule->DuplicateItem( orig_item );

            if( increment && item->Type() == PCB_PAD_T
                && PAD_NAMING::PadCanHaveName( *static_cast<D_PAD*>( dupe_item ) ) )
            {
                PAD_TOOL* padTool = m_toolMgr->GetTool<PAD_TOOL>();
                wxString padName = padTool->GetLastPadName();
                padName = editModule->GetNextPadName( padName );
                padTool->SetLastPadName( padName );
                static_cast<D_PAD*>( dupe_item )->SetName( padName );
            }
        }
        else if( orig_item->GetParent() && orig_item->GetParent()->Type() == PCB_MODULE_T )
        {
            MODULE* parent = static_cast<MODULE*>( orig_item->GetParent() );

            m_commit->Modify( parent );
            dupe_item = parent->DuplicateItem( orig_item, true /* add to parent */ );
        }
        else
        {
            switch( orig_item->Type() )
            {
            case PCB_MODULE_T:
            case PCB_TEXT_T:
            case PCB_LINE_T:
            case PCB_TRACE_T:
            case PCB_VIA_T:
            case PCB_ZONE_AREA_T:
            case PCB_TARGET_T:
            case PCB_DIM_ALIGNED_T:
            case PCB_DIM_CENTER_T:
            case PCB_DIM_ORTHOGONAL_T:
            case PCB_DIM_LEADER_T:
                dupe_item = orig_item->Duplicate();
                break;

            case PCB_GROUP_T:
                dupe_item = static_cast<PCB_GROUP*>( orig_item )->DeepDuplicate();
                break;

            default:
                // Silently drop other items (such as footprint texts) from duplication
                break;
            }
        }

        if( dupe_item )
        {
            if( dupe_item->Type() == PCB_GROUP_T )
            {
                static_cast<PCB_GROUP*>( dupe_item )->RunOnDescendants( [&]( BOARD_ITEM* bItem )
                                                                        {
                                                                            m_commit->Add( bItem );
                                                                        });
            }

            // Clear the selection flag here, otherwise the SELECTION_TOOL
            // will not properly select it later on
            dupe_item->ClearSelected();

            new_items.push_back( dupe_item );
            m_commit->Add( dupe_item );
        }
    }

    // Clear the old selection first
    m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

    // Select the new items
    m_toolMgr->RunAction( PCB_ACTIONS::selectItems, true, &new_items );

    // record the new items as added
    if( !selection.Empty() )
    {
        editFrame->DisplayToolMsg( wxString::Format( _( "Duplicated %d item(s)" ),
                                                     (int) new_items.size() ) );

        // If items were duplicated, pick them up
        // this works well for "dropping" copies around and pushes the commit
        TOOL_EVENT evt = PCB_ACTIONS::move.MakeEvent();
        Move( evt );

        // After moving the new items, we need to refresh the group and view flags
        m_toolMgr->RunAction( PCB_ACTIONS::selectionClear, true );

        if( !is_hover )
            m_toolMgr->RunAction( PCB_ACTIONS::selectItems, true, &new_items );
    }

    return 0;
}


int EDIT_TOOL::CreateArray( const TOOL_EVENT& aEvent )
{
    if( isRouterActive() )
    {
        wxBell();
        return 0;
    }

    const auto& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
            {
                EditToolSelectionFilter( aCollector, EXCLUDE_LOCKED_PADS | EXCLUDE_TRANSIENTS, sTool );
            } );

    if( selection.Empty() )
        return 0;

    // we have a selection to work on now, so start the tool process
    PCB_BASE_FRAME* editFrame = getEditFrame<PCB_BASE_FRAME>();
    ARRAY_CREATOR   array_creator( *editFrame, m_editModules, selection );
    array_creator.Invoke();

    return 0;
}


void EDIT_TOOL::PadFilter( const VECTOR2I&, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
{
    for( int i = aCollector.GetCount() - 1; i >= 0; i-- )
    {
        BOARD_ITEM* item = static_cast<BOARD_ITEM*>( aCollector[i] );

        if( item->Type() != PCB_PAD_T )
            aCollector.Remove( i );
    }
}


void EDIT_TOOL::FootprintFilter( const VECTOR2I&, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool )
{
    for( int i = aCollector.GetCount() - 1; i >= 0; i-- )
    {
        BOARD_ITEM* item = static_cast<BOARD_ITEM*>( aCollector[i] );

        if( item->Type() != PCB_MODULE_T )
            aCollector.Remove( i );
    }
}


bool EDIT_TOOL::updateModificationPoint( PCBNEW_SELECTION& aSelection )
{
    if( m_dragging && aSelection.HasReferencePoint() )
        return false;

    // When there is only one item selected, the reference point is its position...
    if( aSelection.Size() == 1 )
    {
        auto item =  static_cast<BOARD_ITEM*>( aSelection.Front() );
        auto pos = item->GetPosition();
        aSelection.SetReferencePoint( VECTOR2I( pos.x, pos.y ) );
    }
    // ...otherwise modify items with regard to the grid-snapped cursor position
    else
    {
        m_cursor = getViewControls()->GetCursorPosition( true );
        aSelection.SetReferencePoint( m_cursor );
    }

    return true;
}


bool EDIT_TOOL::pickReferencePoint( const wxString& aTooltip, const wxString& aSuccessMessage,
                                    const wxString& aCanceledMessage, VECTOR2I& aReferencePoint )
{
    PCBNEW_PICKER_TOOL* picker = m_toolMgr->GetTool<PCBNEW_PICKER_TOOL>();
    OPT<VECTOR2I>       pickedPoint;
    bool                done = false;

    m_statusPopup->SetText( aTooltip );

    picker->SetClickHandler(
            [&]( const VECTOR2D& aPoint ) -> bool
            {
                pickedPoint = aPoint;

                if( !aSuccessMessage.empty() )
                {
                    m_statusPopup->SetText( aSuccessMessage );
                    m_statusPopup->Expire( 800 );
                }
                else
                {
                    m_statusPopup->Hide();
                }

                return false; // we don't need any more points
            } );

    picker->SetMotionHandler(
            [&]( const VECTOR2D& aPos )
            {
                m_statusPopup->Move( wxGetMousePosition() + wxPoint( 20, -50 ) );
            } );

    picker->SetCancelHandler(
            [&]()
            {
                if( !aCanceledMessage.empty() )
                {
                    m_statusPopup->SetText( aCanceledMessage );
                    m_statusPopup->Expire( 800 );
                }
                else
                {
                    m_statusPopup->Hide();
                }
            } );

    picker->SetFinalizeHandler(
            [&]( const int& aFinalState )
            {
                done = true;
            } );

    m_statusPopup->Move( wxGetMousePosition() + wxPoint( 20, -50 ) );
    m_statusPopup->Popup();

    std::string tool = "";
    m_toolMgr->RunAction( ACTIONS::pickerTool, true, &tool );

    while( !done )
        Wait()->SetPassEvent();

    // Ensure statusPopup is hidden after use and before deleting it:
    m_statusPopup->Hide();

    if( pickedPoint.is_initialized() )
        aReferencePoint = pickedPoint.get();

    return pickedPoint.is_initialized();
}


int EDIT_TOOL::copyToClipboard( const TOOL_EVENT& aEvent )
{
    std::string  tool = "pcbnew.InteractiveEdit.selectReferencePoint";
    CLIPBOARD_IO io;
    GRID_HELPER grid( m_toolMgr, getEditFrame<PCB_BASE_EDIT_FRAME>()->GetMagneticItemsSettings() );

    frame()->PushTool( tool );
    Activate();

    PCBNEW_SELECTION& selection = m_selectionTool->RequestSelection(
            []( const VECTOR2I& aPt, GENERAL_COLLECTOR& aCollector, SELECTION_TOOL* sTool ) {
                EditToolSelectionFilter( aCollector, EXCLUDE_LOCKED_PADS | EXCLUDE_TRANSIENTS, sTool );
            } );

    if( !selection.Empty() )
    {
        m_statusPopup->SetText( _( "Selection copied" ) );
        m_statusPopup->Move( wxGetMousePosition() + wxPoint( 20, 20 ) );

        std::vector<BOARD_ITEM*> items;

        for( EDA_ITEM* item : selection )
            items.push_back( static_cast<BOARD_ITEM*>( item ) );

        VECTOR2I refPoint = grid.BestDragOrigin( getViewControls()->GetCursorPosition( false ), items );
        selection.SetReferencePoint( refPoint );

        io.SetBoard( board() );
        io.SaveSelection( selection, m_editModules );
        m_statusPopup->Expire( 800 );
        m_statusPopup->Show();
    }

    frame()->PopTool( tool );

    return 0;
}


int EDIT_TOOL::cutToClipboard( const TOOL_EVENT& aEvent )
{
    if( !copyToClipboard( aEvent ) )
    {
        // N.B. Setting the CUT flag prevents lock filtering as we only want to delete the items that
        // were copied to the clipboard, no more, no fewer.  Filtering for locked item, if any will be done
        // in the copyToClipboard() routine
        TOOL_EVENT evt( aEvent.Category(), aEvent.Action(), TOOL_ACTION_SCOPE::AS_GLOBAL );
        evt.SetParameter( PCB_ACTIONS::REMOVE_FLAGS::CUT );
        Remove( evt );
    }

    return 0;
}


void EDIT_TOOL::setTransitions()
{
    Go( &EDIT_TOOL::GetAndPlace,         PCB_ACTIONS::getAndPlace.MakeEvent() );
    Go( &EDIT_TOOL::Move,                PCB_ACTIONS::move.MakeEvent() );
    Go( &EDIT_TOOL::Drag,                PCB_ACTIONS::drag45Degree.MakeEvent() );
    Go( &EDIT_TOOL::Drag,                PCB_ACTIONS::dragFreeAngle.MakeEvent() );
    Go( &EDIT_TOOL::Rotate,              PCB_ACTIONS::rotateCw.MakeEvent() );
    Go( &EDIT_TOOL::Rotate,              PCB_ACTIONS::rotateCcw.MakeEvent() );
    Go( &EDIT_TOOL::Flip,                PCB_ACTIONS::flip.MakeEvent() );
    Go( &EDIT_TOOL::Remove,              ACTIONS::doDelete.MakeEvent() );
    Go( &EDIT_TOOL::Remove,              PCB_ACTIONS::deleteFull.MakeEvent() );
    Go( &EDIT_TOOL::Properties,          PCB_ACTIONS::properties.MakeEvent() );
    Go( &EDIT_TOOL::MoveExact,           PCB_ACTIONS::moveExact.MakeEvent() );
    Go( &EDIT_TOOL::MoveWithReference,   PCB_ACTIONS::moveWithReference.MakeEvent() );
    Go( &EDIT_TOOL::Duplicate,           ACTIONS::duplicate.MakeEvent() );
    Go( &EDIT_TOOL::Duplicate,           PCB_ACTIONS::duplicateIncrement.MakeEvent() );
    Go( &EDIT_TOOL::CreateArray,         PCB_ACTIONS::createArray.MakeEvent() );
    Go( &EDIT_TOOL::Mirror,              PCB_ACTIONS::mirror.MakeEvent() );
    Go( &EDIT_TOOL::ChangeTrackWidth,    PCB_ACTIONS::changeTrackWidth.MakeEvent() );

    Go( &EDIT_TOOL::copyToClipboard,     ACTIONS::copy.MakeEvent() );
    Go( &EDIT_TOOL::cutToClipboard,      ACTIONS::cut.MakeEvent() );
}


