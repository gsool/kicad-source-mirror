/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2020 Roberto Fernandez Bautista <roberto.fer.bau@gmail.com>
 * Copyright (C) 2020 KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file cadstar_pcb_archive_loader.cpp
 * @brief Loads a cpa file into a KiCad BOARD object
 */

#include <cadstar_pcb_archive_loader.h>

#include <board_stackup_manager/stackup_predefined_prms.h> // KEY_COPPER, KEY_CORE, KEY_PREPREG
#include <class_board.h>
#include <class_dimension.h>
#include <class_drawsegment.h> // DRAWSEGMENT
#include <class_edge_mod.h>
#include <class_module.h>
#include <class_pcb_text.h>
#include <class_track.h>
#include <class_zone.h>
#include <convert_basic_shapes_to_polygon.h>
#include <trigo.h>

#include <limits> // std::numeric_limits


void CADSTAR_PCB_ARCHIVE_LOADER::Load( ::BOARD* aBoard )
{
    mBoard = aBoard;
    Parse();

    LONGPOINT designLimit = Assignments.Technology.DesignLimit;

    //Note: can't use getKiCadPoint() due wxPoint being int - need long long to make the check
    long long designSizeXkicad   = (long long) designLimit.x * KiCadUnitMultiplier;
    long long designSizeYkicad   = (long long) designLimit.y * KiCadUnitMultiplier;

    // Max size limited by the positive dimension of wxPoint (which is an int)
    long long maxDesignSizekicad = std::numeric_limits<int>::max();

    if( designSizeXkicad > maxDesignSizekicad || designSizeYkicad > maxDesignSizekicad )
        THROW_IO_ERROR( wxString::Format(
                _( "The design is too large and cannot be imported into KiCad. \n"
                   "Please reduce the maximum design size in CADSTAR by navigating to: \n"
                   "Design Tab -> Properties -> Design Options -> Maximum Design Size. \n"
                   "Current Design size: %.2f, %.2f millimeters. \n"
                   "Maximum permitted design size: %.2f, %.2f millimeters.\n" ),
                (double) designSizeXkicad / PCB_IU_PER_MM,
                (double) designSizeYkicad / PCB_IU_PER_MM,
                (double) maxDesignSizekicad / PCB_IU_PER_MM,
                (double) maxDesignSizekicad / PCB_IU_PER_MM ) );

    mDesignCenter =
            ( Assignments.Technology.DesignArea.first + Assignments.Technology.DesignArea.second )
            / 2;

    if( Layout.NetSynch == NETSYNCH::WARNING )
        wxLogWarning(
                _( "The selected file indicates that nets might be out of synchronisation "
                   "with the schematic. It is recommended that you carry out an 'Align Nets' "
                   "procedure in CADSTAR and re-import, to avoid inconsistencies between the "
                   "PCB and the schematic. " ) );

    loadBoardStackup();
    loadDesignRules();
    loadComponentLibrary();
    loadGroups();
    loadBoards();
    loadFigures();
    loadTexts();
    loadDimensions();
    loadAreas();
    loadComponents();
    loadDocumentationSymbols();
    loadTemplates();
    loadCoppers();
    loadNets();

    if( Layout.VariantHierarchy.size() > 0 )
        wxLogWarning(
                _( "The CADSTAR design contains variants which has no KiCad equivalent. All "
                   "components have been loaded on top of each other. " ) );

    wxLogMessage(
            _( "The CADSTAR design has been imported successfully.\n"
               "Please review the import errors and warnings (if any)." ) );
}


void CADSTAR_PCB_ARCHIVE_LOADER::logBoardStackupWarning(
        const wxString& aCadstarLayerName, const PCB_LAYER_ID& aKiCadLayer )
{
    wxLogWarning( wxString::Format(
            _( "The CADSTAR layer '%s' has no KiCad equivalent. All elements on this "
               "layer have been mapped to KiCad layer '%s' instead." ),
            aCadstarLayerName, LSET::Name( aKiCadLayer ) ) );
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadBoardStackup()
{
    std::map<LAYER_ID, LAYER>&       cpaLayers                = Assignments.Layerdefs.Layers;
    std::map<MATERIAL_ID, MATERIAL>& cpaMaterials             = Assignments.Layerdefs.Materials;
    std::vector<LAYER_ID>&           cpaLayerStack            = Assignments.Layerdefs.LayerStack;
    unsigned                         numElecAndPowerLayers    = 0;
    BOARD_DESIGN_SETTINGS&           designSettings           = mBoard->GetDesignSettings();
    BOARD_STACKUP&                   stackup                  = designSettings.GetStackupDescriptor();
    int                              noOfKiCadStackupLayers   = 0;
    int                              lastElectricalLayerIndex = 0;
    int                              dielectricSublayer       = 0;
    int                              numDielectricLayers      = 0;
    bool                             prevWasDielectric        = false;
    BOARD_STACKUP_ITEM*              tempKiCadLayer           = nullptr;
    std::vector<PCB_LAYER_ID>        layerIDs;

    //Remove all layers except required ones
    stackup.RemoveAll();
    layerIDs.push_back( PCB_LAYER_ID::F_CrtYd );
    layerIDs.push_back( PCB_LAYER_ID::B_CrtYd );
    layerIDs.push_back( PCB_LAYER_ID::Margin );
    layerIDs.push_back( PCB_LAYER_ID::Edge_Cuts );
    designSettings.SetEnabledLayers( LSET( &layerIDs[0], layerIDs.size() ) );

    for( auto it = cpaLayerStack.begin(); it != cpaLayerStack.end(); ++it )
    {
        LAYER                   curLayer       = cpaLayers[*it];
        BOARD_STACKUP_ITEM_TYPE kicadLayerType = BOARD_STACKUP_ITEM_TYPE::BS_ITEM_TYPE_UNDEFINED;
        LAYER_T                 copperType     = LAYER_T::LT_UNDEFINED;
        PCB_LAYER_ID            kicadLayerID   = PCB_LAYER_ID::UNDEFINED_LAYER;
        wxString                layerTypeName  = wxEmptyString;

        if( cpaLayers.count( *it ) == 0 )
            wxASSERT_MSG( true, wxT( "Unable to find layer index" ) );

        if( prevWasDielectric && ( curLayer.Type != LAYER_TYPE::CONSTRUCTION ) )
        {
            stackup.Add( tempKiCadLayer ); //only add dielectric layers here after all are done
            dielectricSublayer = 0;
            prevWasDielectric  = false;
            noOfKiCadStackupLayers++;
        }

        switch( curLayer.Type )
        {
        case LAYER_TYPE::ALLDOC:
        case LAYER_TYPE::ALLELEC:
        case LAYER_TYPE::ALLLAYER:
        case LAYER_TYPE::ASSCOMPCOPP:
        case LAYER_TYPE::NOLAYER:
            //Shouldn't be here if CPA file is correctly parsed and not corrupt
            THROW_IO_ERROR( wxString::Format( _( "Unexpected layer '%s' in layer stack." ),
                                              curLayer.Name ) );

        case LAYER_TYPE::JUMPERLAYER:
            copperType     = LAYER_T::LT_JUMPER;
            kicadLayerID   = getKiCadCopperLayerID( ++numElecAndPowerLayers );
            kicadLayerType = BOARD_STACKUP_ITEM_TYPE::BS_ITEM_TYPE_COPPER;
            layerTypeName  = KEY_COPPER;
            break;

        case LAYER_TYPE::ELEC:
            copperType     = LAYER_T::LT_SIGNAL;
            kicadLayerID   = getKiCadCopperLayerID( ++numElecAndPowerLayers );
            kicadLayerType = BOARD_STACKUP_ITEM_TYPE::BS_ITEM_TYPE_COPPER;
            layerTypeName  = KEY_COPPER;
            break;

        case LAYER_TYPE::POWER:
            copperType     = LAYER_T::LT_POWER;
            kicadLayerID   = getKiCadCopperLayerID( ++numElecAndPowerLayers );
            kicadLayerType = BOARD_STACKUP_ITEM_TYPE::BS_ITEM_TYPE_COPPER;
            layerTypeName  = KEY_COPPER;
            mPowerPlaneLayers.push_back( curLayer.ID ); //we will need to add a Copper zone
            break;

        case LAYER_TYPE::CONSTRUCTION:
            kicadLayerID      = PCB_LAYER_ID::UNDEFINED_LAYER;
            kicadLayerType    = BOARD_STACKUP_ITEM_TYPE::BS_ITEM_TYPE_DIELECTRIC;
            prevWasDielectric = true;
            layerTypeName     = KEY_PREPREG;
            //TODO handle KEY_CORE and KEY_PREPREG
            //will need to look at CADSTAR layer embedding (see LAYER->Embedding) to
            //check electrical layers above and below to decide if current layer is prepreg
            // or core
            break;

        case LAYER_TYPE::DOC:

            if( numElecAndPowerLayers > 0 )
                kicadLayerID = PCB_LAYER_ID::Dwgs_User;
            else
                kicadLayerID = PCB_LAYER_ID::Cmts_User;

            logBoardStackupWarning( curLayer.Name, kicadLayerID );
            //TODO: allow user to decide which layer this should be mapped onto.
            break;

        case LAYER_TYPE::NONELEC:
            switch( curLayer.SubType )
            {
            case LAYER_SUBTYPE::LAYERSUBTYPE_ASSEMBLY:

                if( numElecAndPowerLayers > 0 )
                    kicadLayerID = PCB_LAYER_ID::B_Fab;
                else
                    kicadLayerID = PCB_LAYER_ID::F_Fab;

                break;

            case LAYER_SUBTYPE::LAYERSUBTYPE_PLACEMENT:

                if( numElecAndPowerLayers > 0 )
                    kicadLayerID = PCB_LAYER_ID::B_CrtYd;
                else
                    kicadLayerID = PCB_LAYER_ID::F_CrtYd;

                break;

            case LAYER_SUBTYPE::LAYERSUBTYPE_NONE:

                if( curLayer.Name.Lower().Contains( "glue" )
                        || curLayer.Name.Lower().Contains( "adhesive" ) )
                {
                    if( numElecAndPowerLayers > 0 )
                        kicadLayerID = PCB_LAYER_ID::B_Adhes;
                    else
                        kicadLayerID = PCB_LAYER_ID::F_Adhes;

                    wxLogMessage( wxString::Format(
                            _( "The CADSTAR layer '%s' has been assumed to be an adhesive layer. "
                               "All elements on this layer have been mapped to KiCad layer '%s'." ),
                            curLayer.Name, LSET::Name( kicadLayerID ) ) );
                    //TODO: allow user to decide if this is actually an adhesive layer or not.
                }
                else
                {
                    if( numElecAndPowerLayers > 0 )
                        kicadLayerID = PCB_LAYER_ID::Eco2_User;
                    else
                        kicadLayerID = PCB_LAYER_ID::Eco1_User;

                    logBoardStackupWarning( curLayer.Name, kicadLayerID );
                    //TODO: allow user to decide which layer this should be mapped onto.
                }

                break;

            case LAYER_SUBTYPE::LAYERSUBTYPE_PASTE:
                kicadLayerType = BOARD_STACKUP_ITEM_TYPE::BS_ITEM_TYPE_SOLDERPASTE;

                if( numElecAndPowerLayers > 0 )
                {
                    kicadLayerID  = PCB_LAYER_ID::B_Paste;
                    layerTypeName = _HKI( "Bottom Solder Paste" );
                }
                else
                {
                    kicadLayerID  = PCB_LAYER_ID::F_Paste;
                    layerTypeName = _HKI( "Top Solder Paste" );
                }

                break;

            case LAYER_SUBTYPE::LAYERSUBTYPE_SILKSCREEN:
                kicadLayerType = BOARD_STACKUP_ITEM_TYPE::BS_ITEM_TYPE_SILKSCREEN;

                if( numElecAndPowerLayers > 0 )
                {
                    kicadLayerID  = PCB_LAYER_ID::B_SilkS;
                    layerTypeName = _HKI( "Bottom Silk Screen" );
                }
                else
                {
                    kicadLayerID  = PCB_LAYER_ID::F_SilkS;
                    layerTypeName = _HKI( "Top Silk Screen" );
                }

                break;

            case LAYER_SUBTYPE::LAYERSUBTYPE_SOLDERRESIST:
                kicadLayerType = BOARD_STACKUP_ITEM_TYPE::BS_ITEM_TYPE_SOLDERMASK;

                if( numElecAndPowerLayers > 0 )
                {
                    kicadLayerID  = PCB_LAYER_ID::B_Mask;
                    layerTypeName = _HKI( "Bottom Solder Mask" );
                }
                else
                {
                    kicadLayerID  = PCB_LAYER_ID::F_Mask;
                    layerTypeName = _HKI( "Top Solder Mask" );
                }

                break;

            default:
                wxASSERT_MSG( true, wxT( "Unknown CADSTAR Layer Sub-type" ) );
                break;
            }
            break;

        default:
            wxASSERT_MSG( true, wxT( "Unknown CADSTAR Layer Type" ) );
            break;
        }

        mLayermap.insert( std::make_pair( curLayer.ID, kicadLayerID ) );

        if( dielectricSublayer == 0 )
            tempKiCadLayer = new BOARD_STACKUP_ITEM( kicadLayerType );

        tempKiCadLayer->SetLayerName( curLayer.Name );
        tempKiCadLayer->SetBrdLayerId( kicadLayerID );

        if( prevWasDielectric )
        {
            wxASSERT_MSG( kicadLayerID == PCB_LAYER_ID::UNDEFINED_LAYER,
                    wxT( "Error Processing Dielectric Layer. "
                         "Expected to have undefined layer type" ) );

            if( dielectricSublayer == 0 )
                tempKiCadLayer->SetDielectricLayerId( ++numDielectricLayers );
            else
                tempKiCadLayer->AddDielectricPrms( dielectricSublayer );
        }

        if( curLayer.MaterialId != UNDEFINED_MATERIAL_ID )
        {
            tempKiCadLayer->SetMaterial(
                    cpaMaterials[curLayer.MaterialId].Name, dielectricSublayer );
            tempKiCadLayer->SetEpsilonR( cpaMaterials[curLayer.MaterialId].Permittivity.GetDouble(),
                    dielectricSublayer );
            tempKiCadLayer->SetLossTangent(
                    cpaMaterials[curLayer.MaterialId].LossTangent.GetDouble(), dielectricSublayer );
            //TODO add Resistivity when KiCad supports it
        }

        tempKiCadLayer->SetThickness(
                curLayer.Thickness * KiCadUnitMultiplier, dielectricSublayer );

        if( layerTypeName != wxEmptyString )
            tempKiCadLayer->SetTypeName( layerTypeName );

        if( !prevWasDielectric )
        {
            stackup.Add( tempKiCadLayer ); //only add non-dielectric layers here
            ++noOfKiCadStackupLayers;
            layerIDs.push_back( tempKiCadLayer->GetBrdLayerId() );
            designSettings.SetEnabledLayers( LSET( &layerIDs[0], layerIDs.size() ) );
        }
        else
            ++dielectricSublayer;

        if( copperType != LAYER_T::LT_UNDEFINED )
        {
            wxASSERT( mBoard->SetLayerType( tempKiCadLayer->GetBrdLayerId(),
                    copperType ) ); //move to outside, need to enable layer in board first
            lastElectricalLayerIndex = noOfKiCadStackupLayers - 1;
            wxASSERT( mBoard->SetLayerName(
                    tempKiCadLayer->GetBrdLayerId(), tempKiCadLayer->GetLayerName() ) );
            //TODO set layer names for other CADSTAR layers when KiCad supports custom
            //layer names on non-copper layers
            mCopperLayers.insert( std::make_pair( curLayer.PhysicalLayer, curLayer.ID ) );
        }
    }

    //change last copper layer to be B_Cu instead of an inner layer
    LAYER_ID     cadstarlastElecLayer = mCopperLayers.rbegin()->second;
    PCB_LAYER_ID lastElecBrdId =
            stackup.GetStackupLayer( lastElectricalLayerIndex )->GetBrdLayerId();
    std::remove( layerIDs.begin(), layerIDs.end(), lastElecBrdId );
    layerIDs.push_back( PCB_LAYER_ID::B_Cu );
    tempKiCadLayer = stackup.GetStackupLayer( lastElectricalLayerIndex );
    tempKiCadLayer->SetBrdLayerId( PCB_LAYER_ID::B_Cu );
    wxASSERT( mBoard->SetLayerName(
            tempKiCadLayer->GetBrdLayerId(), tempKiCadLayer->GetLayerName() ) );
    mLayermap.at( cadstarlastElecLayer ) = PCB_LAYER_ID::B_Cu;

    //make all layers enabled and visible
    mBoard->SetEnabledLayers( LSET( &layerIDs[0], layerIDs.size() ) );
    mBoard->SetVisibleLayers( LSET( &layerIDs[0], layerIDs.size() ) );

    mBoard->SetCopperLayerCount( numElecAndPowerLayers );
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadDesignRules()
{
    BOARD_DESIGN_SETTINGS&                 ds = mBoard->GetDesignSettings();
    std::map<SPACINGCODE_ID, SPACINGCODE>& spacingCodes   = Assignments.Codedefs.SpacingCodes;

    auto applyRule =
        [&]( wxString aID, int* aVal )
        {
            if( spacingCodes.find( aID ) == spacingCodes.end() )
                wxLogWarning( _( "Design rule %s was not found. This was ignored." ) );
            else
                *aVal = getKiCadLength( spacingCodes.at( aID ).Spacing );
        };

    //Note: for details on the different spacing codes see SPACINGCODE::ID

    applyRule( "T_T", &ds.m_MinClearance );
    applyRule( "C_B", &ds.m_CopperEdgeClearance );
    applyRule( "H_H", &ds.m_HoleToHoleMin );

    ds.m_TrackMinWidth = Assignments.Technology.MinRouteWidth;

    auto applyNetClassRule =
        [&]( wxString aID, ::NETCLASS* aNetClassPtr, void (::NETCLASS::*aFunc)(int) )
        {
            int value = -1;
            applyRule(aID, &value);

            if( value != -1 )
                (aNetClassPtr->*aFunc)(value);

        };

    applyNetClassRule( "T_T", ds.GetDefault(), &::NETCLASS::SetClearance );

    wxLogWarning( _( "KiCad design rules are different from CADSTAR ones. Only the compatible "
                     "design rules were imported. It is recommended that you review the design "
                     "rules that have been applied." ) );
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadComponentLibrary()
{
    for( std::pair<SYMDEF_ID, SYMDEF> symPair : Library.ComponentDefinitions )
    {
        SYMDEF_ID key        = symPair.first;
        SYMDEF    component  = symPair.second;
        wxString  moduleName = component.ReferenceName
                              + ( ( component.Alternate.size() > 0 ) ?
                                              ( wxT( " (" ) + component.Alternate + wxT( ")" ) ) :
                                              wxT( "" ) );
        MODULE* m = new MODULE( mBoard );
        m->SetPosition( getKiCadPoint( component.Origin ) );

        LIB_ID libID;
        libID.Parse( moduleName, LIB_ID::LIB_ID_TYPE::ID_PCB, true );

        m->SetFPID( libID );
        loadLibraryFigures( component, m );
        loadLibraryCoppers( component, m );
        loadLibraryAreas( component, m );
        loadLibraryPads( component, m );

        mLibraryMap.insert( std::make_pair( key, m ) );
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadLibraryFigures( const SYMDEF& aComponent, MODULE* aModule )
{
    for( std::pair<FIGURE_ID, FIGURE> figPair : aComponent.Figures )
    {
        FIGURE& fig = figPair.second;
        drawCadstarShape( fig.Shape, getKiCadLayer( fig.LayerID ),
                getLineThickness( fig.LineCodeID ),
                wxString::Format( "Component %s:%s -> Figure %s", aComponent.ReferenceName,
                        aComponent.Alternate, fig.ID ),
                aModule );
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadLibraryCoppers( const SYMDEF& aComponent, MODULE* aModule )
{
    for( COMPONENT_COPPER compCopper : aComponent.ComponentCoppers )
    {
        int lineThickness = getKiCadLength( getCopperCode( compCopper.CopperCodeID ).CopperWidth );

        drawCadstarShape( compCopper.Shape, getKiCadLayer( compCopper.LayerID ), lineThickness,
                wxString::Format( "Component %s:%s -> Copper element", aComponent.ReferenceName,
                        aComponent.Alternate ),
                aModule );
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadLibraryAreas( const SYMDEF& aComponent, MODULE* aModule )
{
    for( std::pair<COMP_AREA_ID, COMPONENT_AREA> areaPair : aComponent.ComponentAreas )
    {
        COMPONENT_AREA& area = areaPair.second;

        if( area.NoVias || area.NoTracks )
        {
            ZONE_CONTAINER* zone =
                    getZoneFromCadstarShape( area.Shape, getLineThickness( area.LineCodeID ), aModule );

            aModule->Add( zone, ADD_MODE::APPEND );

            if( isLayerSet( area.LayerID ) )
                zone->SetLayerSet( getKiCadLayerSet( area.LayerID ) );
            else
                zone->SetLayer( getKiCadLayer( area.LayerID ) );

            zone->SetIsRuleArea( true );       //import all CADSTAR areas as Keepout zones
            zone->SetDoNotAllowPads( false ); //no CADSTAR equivalent
            zone->SetZoneName( area.ID );

            //There is no distinction between tracks and copper pours in CADSTAR Keepout zones
            zone->SetDoNotAllowTracks( area.NoTracks );
            zone->SetDoNotAllowCopperPour( area.NoTracks );

            zone->SetDoNotAllowVias( area.NoVias );
        }
        else
        {
            wxString libName = aComponent.ReferenceName;

            if( !aComponent.Alternate.IsEmpty() )
                libName << wxT( " (" ) << aComponent.Alternate << wxT( ")" );

            wxLogError(
                    wxString::Format( _( "The CADSTAR area '%s' in library component '%s' does not "
                                         "have a KiCad equivalent. The area is neither a via or"
                                         "route keepout area. The area was not imported. " ),
                            area.ID, libName ) );
        }
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadLibraryPads( const SYMDEF& aComponent, MODULE* aModule )
{
    for( std::pair<PAD_ID, PAD> padPair : aComponent.Pads )
    {
        PAD&    csPad     = padPair.second; //Cadstar pad
        PADCODE csPadcode = getPadCode( csPad.PadCodeID );

        D_PAD* pad = new D_PAD( aModule );
        aModule->Add( pad, ADD_MODE::INSERT ); // insert so that we get correct behaviour when finding pads
                                               // in the module by PAD_ID - see loadNets()

        switch( csPad.Side )
        {
        case PAD_SIDE::MAXIMUM: //Bottom side
            pad->SetAttribute( PAD_ATTR_T::PAD_ATTRIB_SMD );
            pad->SetLayerSet( LSET( 3, B_Cu, B_Paste, B_Mask ) );
            break;

        case PAD_SIDE::MINIMUM: //TOP side
            pad->SetAttribute( PAD_ATTR_T::PAD_ATTRIB_SMD );
            pad->SetLayerSet( LSET( 3, F_Cu, F_Paste, F_Mask ) );
            break;

        case PAD_SIDE::THROUGH_HOLE:

            if( csPadcode.Plated )
                pad->SetAttribute( PAD_ATTR_T::PAD_ATTRIB_STANDARD );
            else
                pad->SetAttribute( PAD_ATTR_T::PAD_ATTRIB_HOLE_NOT_PLATED );

            pad->SetLayerSet( pad->StandardMask() ); // for now we will assume no paste layers
            //TODO: We need to read the csPad->Reassigns vector to make sure no paste
            break;

        default:
            wxASSERT_MSG( true, "Unknown Pad type" );
        }

        pad->SetName( csPad.Identifier.IsEmpty() ? wxString::Format( wxT( "%i" ), csPad.ID ) :
                                                   csPad.Identifier );

        pad->SetPos0( getKiCadPoint( csPad.Position ) - aModule->GetPosition() );
        pad->SetOrientation( getAngleTenthDegree( csPad.OrientAngle ) );

        if( csPadcode.Shape.Size == 0 )
            // zero sized pads seems to break KiCad so lets make it very small instead
            csPadcode.Shape.Size = 1;

        switch( csPadcode.Shape.ShapeType )
        {
        case PAD_SHAPE_TYPE::ANNULUS:
            //todo fix: use custom shape instead (Donught shape, i.e. a circle with a hole)
            pad->SetShape( PAD_SHAPE_T::PAD_SHAPE_CIRCLE );
            pad->SetSize( { getKiCadLength( csPadcode.Shape.Size ),
                    getKiCadLength( csPadcode.Shape.Size ) } );
            break;

        case PAD_SHAPE_TYPE::BULLET:
            pad->SetShape( PAD_SHAPE_T::PAD_SHAPE_CHAMFERED_RECT );
            pad->SetSize( { getKiCadLength( (long long) csPadcode.Shape.Size
                                            + (long long) csPadcode.Shape.LeftLength
                                            + (long long) csPadcode.Shape.RightLength ),
                    getKiCadLength( csPadcode.Shape.Size ) } );
            pad->SetChamferPositions( RECT_CHAMFER_POSITIONS::RECT_CHAMFER_BOTTOM_LEFT
                                      | RECT_CHAMFER_POSITIONS::RECT_CHAMFER_TOP_LEFT );
            pad->SetRoundRectRadiusRatio( 0.5 );
            pad->SetChamferRectRatio( 0.0 );
            break;

        case PAD_SHAPE_TYPE::CIRCLE:
            pad->SetShape( PAD_SHAPE_T::PAD_SHAPE_CIRCLE );
            pad->SetSize( { getKiCadLength( csPadcode.Shape.Size ),
                    getKiCadLength( csPadcode.Shape.Size ) } );
            break;

        case PAD_SHAPE_TYPE::DIAMOND:
            pad->SetShape( PAD_SHAPE_T::PAD_SHAPE_RECT );
            pad->SetOrientation( pad->GetOrientation() + 450.0 ); // rotate 45deg anticlockwise
            pad->SetSize( { getKiCadLength( csPadcode.Shape.Size ),
                    getKiCadLength( csPadcode.Shape.Size ) } );
            break;

        case PAD_SHAPE_TYPE::FINGER:
            pad->SetShape( PAD_SHAPE_T::PAD_SHAPE_OVAL );
            pad->SetSize( { getKiCadLength( (long long) csPadcode.Shape.Size
                                            + (long long) csPadcode.Shape.LeftLength
                                            + (long long) csPadcode.Shape.RightLength ),
                    getKiCadLength( csPadcode.Shape.Size ) } );
            break;

        case PAD_SHAPE_TYPE::OCTAGON:
            pad->SetShape( PAD_SHAPE_CHAMFERED_RECT );
            pad->SetChamferPositions( RECT_CHAMFER_POSITIONS::RECT_CHAMFER_ALL );
            pad->SetChamferRectRatio( 0.25 );
            pad->SetSize( { getKiCadLength( csPadcode.Shape.Size ),
                    getKiCadLength( csPadcode.Shape.Size ) } );
            break;

        case PAD_SHAPE_TYPE::RECTANGLE:
            pad->SetShape( PAD_SHAPE_T::PAD_SHAPE_RECT );
            pad->SetSize( { getKiCadLength( (long long) csPadcode.Shape.Size
                                            + (long long) csPadcode.Shape.LeftLength
                                            + (long long) csPadcode.Shape.RightLength ),
                    getKiCadLength( csPadcode.Shape.Size ) } );
            break;

        case PAD_SHAPE_TYPE::ROUNDED_RECT:
            pad->SetShape( PAD_SHAPE_T::PAD_SHAPE_RECT );
            pad->SetRoundRectCornerRadius( getKiCadLength( csPadcode.Shape.InternalFeature ) );
            pad->SetSize( { getKiCadLength( (long long) csPadcode.Shape.Size
                                            + (long long) csPadcode.Shape.LeftLength
                                            + (long long) csPadcode.Shape.RightLength ),
                    getKiCadLength( csPadcode.Shape.Size ) } );
            break;


        case PAD_SHAPE_TYPE::SQUARE:
            pad->SetShape( PAD_SHAPE_T::PAD_SHAPE_RECT );
            pad->SetSize( { getKiCadLength( csPadcode.Shape.Size ),
                    getKiCadLength( csPadcode.Shape.Size ) } );
            break;

        default:
            wxASSERT_MSG( true, "Unknown Pad Shape" );
        }

        if( csPadcode.ReliefClearance != UNDEFINED_VALUE )
            pad->SetThermalGap( getKiCadLength( csPadcode.ReliefClearance ) );

        if( csPadcode.ReliefWidth != UNDEFINED_VALUE )
            pad->SetThermalSpokeWidth( getKiCadLength( csPadcode.ReliefWidth ) );

        pad->SetOrientation( pad->GetOrientation() + getAngleTenthDegree( csPadcode.Shape.OrientAngle ) );

        if( csPadcode.DrillDiameter != UNDEFINED_VALUE )
        {
            if( csPadcode.SlotLength != UNDEFINED_VALUE )
            {
                pad->SetDrillSize( { getKiCadLength( csPadcode.DrillDiameter ),
                                     getKiCadLength( (long long) csPadcode.DrillOversize
                                      + (long long) csPadcode.DrillDiameter ) } );
            }
            else
            {
                pad->SetDrillSize( { getKiCadLength( csPadcode.DrillDiameter ),
                                     getKiCadLength( csPadcode.DrillDiameter ) } );
            }
        }
        //TODO handle csPadcode.Reassigns when KiCad supports full padstacks
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadGroups()
{
    for( std::pair<GROUP_ID, GROUP> groupPair : Layout.Groups )
    {
        GROUP& csGroup = groupPair.second;

        PCB_GROUP* kiGroup = new PCB_GROUP( mBoard );

        mBoard->Add( kiGroup );
        kiGroup->SetName( csGroup.Name );
        kiGroup->SetLocked( csGroup.Fixed );

        mGroupMap.insert( { csGroup.ID, kiGroup } );
    }

    //now add any groups to their parent group
    for( std::pair<GROUP_ID, GROUP> groupPair : Layout.Groups )
    {
        GROUP& csGroup = groupPair.second;

        if( !csGroup.GroupID.IsEmpty() )
        {
            if( mGroupMap.find( csGroup.ID ) == mGroupMap.end() )
            {
                THROW_IO_ERROR( wxString::Format(
                        _( "The file appears to be corrupt. Unable to find group ID %s "
                           "in the group definitions." ),
                        csGroup.ID ) );
            }
            else if( mGroupMap.find( csGroup.ID ) == mGroupMap.end() )
            {
                THROW_IO_ERROR( wxString::Format(
                        _( "The file appears to be corrupt. Unable to find sub group %s "
                           "in the group map (parent group ID=%s, Name=%s)." ),
                        csGroup.GroupID, csGroup.ID, csGroup.Name ) );
            }
            else
            {
                PCB_GROUP* kiCadGroup = mGroupMap.at( csGroup.ID );
                PCB_GROUP* parentGroup = mGroupMap.at( csGroup.GroupID );
                parentGroup->AddItem( kiCadGroup );
            }
        }
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadBoards()
{
    for( std::pair<BOARD_ID, BOARD> boardPair : Layout.Boards )
    {
        BOARD& board = boardPair.second;
        GROUP_ID boardGroup = createUniqueGroupID( wxT( "Board" ) );
        drawCadstarShape( board.Shape, PCB_LAYER_ID::Edge_Cuts,
                getLineThickness( board.LineCodeID ), wxString::Format( "BOARD %s", board.ID ),
                mBoard, boardGroup );

        if( !board.GroupID.IsEmpty() )
        {
            addToGroup( board.GroupID, getKiCadGroup( boardGroup ) );
        }

        //TODO process board attributes when KiCad supports them
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadFigures()
{
    for( std::pair<FIGURE_ID, FIGURE> figPair : Layout.Figures )
    {
        FIGURE& fig = figPair.second;
        drawCadstarShape( fig.Shape, getKiCadLayer( fig.LayerID ),
                getLineThickness( fig.LineCodeID ), wxString::Format( "FIGURE %s", fig.ID ), mBoard,
                fig.GroupID );

        //TODO process "swaprule" (doesn't seem to apply to Layout Figures?)
        //TODO process re-use block when KiCad Supports it
        //TODO process attributes when KiCad Supports attributes in figures
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadTexts()
{
    for( std::pair<TEXT_ID, TEXT> txtPair : Layout.Texts )
    {
        TEXT& csTxt = txtPair.second;
        drawCadstarText( csTxt, mBoard );
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadDimensions()
{
    for( std::pair<DIMENSION_ID, DIMENSION> dimPair : Layout.Dimensions )
    {
        DIMENSION& csDim = dimPair.second;

        switch( csDim.Type )
        {
        case DIMENSION::TYPE::LINEARDIM:
            switch( csDim.Subtype )
            {
            case DIMENSION::SUBTYPE::DIRECT:
            case DIMENSION::SUBTYPE::ORTHOGONAL:
            {
                ::ALIGNED_DIMENSION* dimension = new ::ALIGNED_DIMENSION( mBoard );
                TEXTCODE             dimText   = getTextCode( csDim.Text.TextCodeID );
                mBoard->Add( dimension, ADD_MODE::APPEND );

                dimension->SetLayer( getKiCadLayer( csDim.LayerID ) );
                dimension->SetPrecision( csDim.Precision );
                dimension->SetStart( getKiCadPoint( csDim.Line.Start ) );
                dimension->SetEnd( getKiCadPoint( csDim.Line.End ) );
                dimension->Text().SetTextThickness( getKiCadLength( dimText.LineWidth ) );
                dimension->Text().SetTextSize( wxSize(
                        getKiCadLength( dimText.Width ), getKiCadLength( dimText.Height ) ) );

                switch( csDim.LinearUnits )
                {
                case UNITS::METER:
                case UNITS::CENTIMETER:
                case UNITS::MM:
                case UNITS::MICROMETRE:
                    dimension->SetUnits( EDA_UNITS::MILLIMETRES, false );
                    break;

                case UNITS::INCH:
                    dimension->SetUnits( EDA_UNITS::INCHES, false );
                    break;

                case UNITS::THOU:
                    dimension->SetUnits( EDA_UNITS::INCHES, true );
                    break;
                }
            }
                continue;

            default: //all others
                wxLogError( wxString::Format(
                        _( "Dimension ID %s has no KiCad equivalent. This was not imported" ),
                        csDim.ID ) );
                break;
            }
            break;

        case DIMENSION::TYPE::ANGLEDIM:
        case DIMENSION::TYPE::LEADERDIM:
        default:
            wxLogError( wxString::Format(
                    _( "Dimension ID %s has no KiCad equivalent. This was not imported" ),
                    csDim.ID ) );
            break;
        }
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadAreas()
{
    for( std::pair<AREA_ID, AREA> areaPair : Layout.Areas )
    {
        AREA& area = areaPair.second;

        if( area.NoVias || area.NoTracks || area.Keepout )
        {
            ZONE_CONTAINER* zone = getZoneFromCadstarShape(
                    area.Shape, getLineThickness( area.LineCodeID ), mBoard );

            mBoard->Add( zone, ADD_MODE::APPEND );

            if( isLayerSet( area.LayerID ) )
                zone->SetLayerSet( getKiCadLayerSet( area.LayerID ) );
            else
                zone->SetLayer( getKiCadLayer( area.LayerID ) );

            zone->SetIsRuleArea( true );       //import all CADSTAR areas as Keepout zones
            zone->SetDoNotAllowPads( false ); //no CADSTAR equivalent
            zone->SetZoneName( area.Name );

            zone->SetDoNotAllowFootprints( area.Keepout );

            zone->SetDoNotAllowTracks( area.NoTracks );
            zone->SetDoNotAllowCopperPour( area.NoTracks );

            zone->SetDoNotAllowVias( area.NoVias );

            if( area.Placement || area.Routing )
                wxLogWarning( wxString::Format(
                        _( "The CADSTAR area '%s' is defined as a placement and/or routing area "
                           "in CADSTAR, in addition to Keepout. Placement or Routing areas are "
                           "not supported in KiCad. Only the supported elements were imported." ),
                        area.Name ) );
        }
        else
        {
            wxLogError(
                    wxString::Format( _( "The CADSTAR area '%s' does not have a KiCad equivalent. "
                                         "Pure Placement or Routing areas are not supported." ),
                            area.Name ) );
        }

        //todo Process area.AreaHeight when KiCad supports 3D design rules
        //TODO process attributes
        //TODO process addition to a group
        //TODO process "swaprule"
        //TODO process re-use block
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadComponents()
{
    for( std::pair<COMPONENT_ID, COMPONENT> compPair : Layout.Components )
    {
        COMPONENT& comp = compPair.second;

        auto moduleIter = mLibraryMap.find( comp.SymdefID );

        if( moduleIter == mLibraryMap.end() )
        {
            THROW_IO_ERROR( wxString::Format( _( "Unable to find component '%s' in the library"
                                                 "(Symdef ID: '%s')" ),
                    comp.Name, comp.SymdefID ) );
        }

        // copy constructor to clone the module from the library
        MODULE* m                      = new MODULE( *moduleIter->second );
        const_cast<KIID&>( m->m_Uuid ) = KIID();

        mBoard->Add( m, ADD_MODE::APPEND );

        //set to empty string to avoid duplication when loading attributes:
        m->SetValue( wxEmptyString );

        m->SetPosition( getKiCadPoint( comp.Origin ) );
        m->SetOrientation( getAngleTenthDegree( comp.OrientAngle ) );
        m->SetReference( comp.Name );

        if( comp.Mirror )
        {
            m->Flip( getKiCadPoint( comp.Origin ), false );
            m->SetOrientation( m->GetOrientation() + 1800.0 );
        }

        loadComponentAttributes( comp, m );

        if( !comp.PartID.IsEmpty() && comp.PartID != wxT( "NO_PART" ) )
            m->SetDescription( getPart( comp.PartID ).Definition.Name );

        mComponentMap.insert( { comp.ID, m } );
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadDocumentationSymbols()
{
    //No KiCad equivalent. Loaded as graphic and text elements instead

    for( std::pair<DOCUMENTATION_SYMBOL_ID, DOCUMENTATION_SYMBOL> docPair :
            Layout.DocumentationSymbols )
    {
        DOCUMENTATION_SYMBOL& docSymInstance = docPair.second;


        auto docSymIter = Library.ComponentDefinitions.find( docSymInstance.SymdefID );

        if( docSymIter == Library.ComponentDefinitions.end() )
        {
            THROW_IO_ERROR( wxString::Format( _( "Unable to find documentation symbol in the "
                                                 "library (Symdef ID: '%s')" ),
                    docSymInstance.SymdefID ) );
        }

        SYMDEF&    docSymDefinition = ( *docSymIter ).second;
        wxPoint moveVector =
                getKiCadPoint( docSymInstance.Origin ) - getKiCadPoint( docSymDefinition.Origin );
        double     rotationAngle = getAngleTenthDegree( docSymInstance.OrientAngle );
        double     scalingFactor =
                (double) docSymInstance.ScaleRatioNumerator / (double) docSymInstance.ScaleRatioDenominator;
        wxPoint centreOfTransform = getKiCadPoint( docSymDefinition.Origin );
        bool    mirrorInvert      = docSymInstance.Mirror;

        //create a group to store the items in
        wxString groupName = docSymDefinition.ReferenceName;

        if( !docSymDefinition.Alternate.IsEmpty() )
            groupName += wxT( " (" ) + docSymDefinition.Alternate + wxT( ")" );

        GROUP_ID groupID = createUniqueGroupID( groupName );

        LSEQ layers = getKiCadLayerSet( docSymInstance.LayerID ).Seq();

        for( PCB_LAYER_ID layer : layers )
        {
            for( std::pair<FIGURE_ID, FIGURE> figPair : docSymDefinition.Figures )
            {
                FIGURE fig = figPair.second;
                drawCadstarShape( fig.Shape, layer, getLineThickness( fig.LineCodeID ),
                        wxString::Format( "DOCUMENTATION SYMBOL %s, FIGURE %s",
                                docSymDefinition.ReferenceName, fig.ID ),
                        mBoard, groupID, moveVector, rotationAngle, scalingFactor,
                        centreOfTransform, mirrorInvert );
            }
        }

        for( std::pair<TEXT_ID, TEXT> textPair : docSymDefinition.Texts )
        {
            TEXT txt = textPair.second;
            drawCadstarText( txt, mBoard, groupID, docSymInstance.LayerID, moveVector, rotationAngle,
                    scalingFactor, centreOfTransform, mirrorInvert );
        }
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadTemplates()
{
    for( std::pair<TEMPLATE_ID, TEMPLATE> tempPair : Layout.Templates )
    {
        TEMPLATE& csTemplate = tempPair.second;

        ZONE_CONTAINER* zone = getZoneFromCadstarShape(
                csTemplate.Shape, getLineThickness( csTemplate.LineCodeID ), mBoard );

        mBoard->Add( zone, ADD_MODE::APPEND );

        zone->SetZoneName( csTemplate.Name );
        zone->SetLayer( getKiCadLayer( csTemplate.LayerID ) );

        if( !(csTemplate.NetID.IsEmpty() || csTemplate.NetID == wxT("NONE") ) )
            zone->SetNet( getKiCadNet( csTemplate.NetID ) );

        if( csTemplate.Pouring.AllowInNoRouting )
            wxLogError( wxString::Format(
                    _( "The CADSTAR template '%s' has the setting 'Allow in No Routing Areas' "
                       "enabled. This setting has no KiCad equivalent, so it has been ignored." ),
                    csTemplate.Name ) );

        if( csTemplate.Pouring.BoxIsolatedPins )
            wxLogError( wxString::Format(
                    _( "The CADSTAR template '%s' has the setting 'Box Isolated Pins'"
                       "enabled. This setting has no KiCad equivalent, so it has been ignored." ),
                    csTemplate.Name ) );

        if( csTemplate.Pouring.AutomaticRepour )
            wxLogWarning( wxString::Format(
                    _( "The CADSTAR template '%s' has the setting 'Automatic Repour'"
                       "enabled. This setting has no KiCad equivalent, so it has been ignored." ),
                    csTemplate.Name ) );

        // Sliver width has different behaviour to KiCad Zone's minimum thickness
        // In Cadstar 'Sliver width' has to be greater than the Copper thickness, whereas in
        // Kicad it is the opposite.
        if( csTemplate.Pouring.SliverWidth != 0 )
            wxLogError( wxString::Format(
                    _( "The CADSTAR template '%s' has a non-zero value defined for the "
                       "'Sliver Width' setting. There is no KiCad equivalent for "
                       "this, so this setting was ignored." ),
                    csTemplate.Name ) );


        if( csTemplate.Pouring.MinIsolatedCopper != csTemplate.Pouring.MinDisjointCopper )
            wxLogError( wxString::Format(
                    _( "The CADSTAR template '%s' has different settings for 'Retain Poured Copper "
                       "- Disjoint' and 'Retain Poured Copper - Isolated'. KiCad does not "
                       "distinguish between these two settings. The setting for disjoint copper "
                       "has been applied as the minimum island area of the KiCad Zone." ),
                    csTemplate.Name ) );

        if( csTemplate.Pouring.MinDisjointCopper < 0 )
            zone->SetMinIslandArea( -1 );
        else
            zone->SetMinIslandArea(
                    (long long) getKiCadLength( csTemplate.Pouring.MinDisjointCopper )
                    * (long long) getKiCadLength( csTemplate.Pouring.MinDisjointCopper ) );

        zone->SetLocalClearance( getKiCadLength( csTemplate.Pouring.AdditionalIsolation ) );

        if( csTemplate.Pouring.FillType == TEMPLATE::POURING::COPPER_FILL_TYPE::HATCHED )
        {
            zone->SetFillMode( ZONE_FILL_MODE::HATCH_PATTERN );
            zone->SetHatchGap( getKiCadHatchCodeGap( csTemplate.Pouring.HatchCodeID ) );
            zone->SetHatchThickness( getKiCadHatchCodeThickness( csTemplate.Pouring.HatchCodeID ) );
            zone->SetHatchOrientation( getHatchCodeAngleDegrees( csTemplate.Pouring.HatchCodeID ) );
        }
        else
        {
            zone->SetFillMode( ZONE_FILL_MODE::POLYGONS );
        }

        if( csTemplate.Pouring.ThermalReliefOnPads != csTemplate.Pouring.ThermalReliefOnVias
                || csTemplate.Pouring.ThermalReliefPadsAngle
                           != csTemplate.Pouring.ThermalReliefViasAngle )
            wxLogWarning( wxString::Format(
                    _( "The CADSTAR template '%s' has different settings for thermal relief "
                       "in pads and vias. KiCad only supports one single setting for both. The "
                       "setting for pads has been applied." ),
                    csTemplate.Name ) );

        if( csTemplate.Pouring.ThermalReliefOnPads )
        {
            zone->SetThermalReliefGap( getKiCadLength( csTemplate.Pouring.ClearanceWidth ) );
            zone->SetThermalReliefSpokeWidth( getKiCadLength(
                    getCopperCode( csTemplate.Pouring.ReliefCopperCodeID ).CopperWidth ));
            zone->SetPadConnection( ZONE_CONNECTION::THERMAL );
        }
        else
            zone->SetPadConnection( ZONE_CONNECTION::FULL );
    }

    //Now create power plane layers:
    for( LAYER_ID layer : mPowerPlaneLayers )
    {
        wxASSERT( Assignments.Layerdefs.Layers.find( layer ) != Assignments.Layerdefs.Layers.end() );

        //The net name will equal the layer name
        wxString powerPlaneLayerName = Assignments.Layerdefs.Layers.at( layer ).Name;
        NET_ID netid = wxEmptyString;

        for( std::pair<NET_ID, NET> netPair : Layout.Nets )
        {
            NET net = netPair.second;

            if( net.Name == powerPlaneLayerName )
            {
                netid = net.ID;
                break;
            }
        }

        if( netid.IsEmpty() )
        {
            wxLogError( wxString::Format(
                    _( "The CADSTAR layer '%s' is defined as a power plane layer. However no "
                       "net with such name exists. The layer has been loaded but no copper zone "
                       "was created." ),
                    powerPlaneLayerName ) );
        }
        else
        {
            for( std::pair<BOARD_ID, BOARD> boardPair : Layout.Boards )
            {
                //create a zone in each board shape
                BOARD& board = boardPair.second;
                int    defaultLineThicknesss =
                        mBoard->GetDesignSettings().GetLineThickness( PCB_LAYER_ID::Edge_Cuts );
                ZONE_CONTAINER* zone =
                        getZoneFromCadstarShape( board.Shape, defaultLineThicknesss, mBoard );

                mBoard->Add( zone, ADD_MODE::APPEND );

                zone->SetZoneName( powerPlaneLayerName );
                zone->SetLayer( getKiCadLayer( layer ) );
                zone->SetFillMode( ZONE_FILL_MODE::POLYGONS );
                zone->SetPadConnection( ZONE_CONNECTION::FULL );
                zone->SetMinIslandArea( -1 );
                zone->SetNet( getKiCadNet( netid ) );
            }
        }



    }

}


void CADSTAR_PCB_ARCHIVE_LOADER::loadCoppers()
{
    for( std::pair<COPPER_ID, COPPER> copPair : Layout.Coppers )
    {
        COPPER& csCopper = copPair.second;

        if( !csCopper.PouredTemplateID.IsEmpty() )
            continue; //ignore copper related to a template as we've already loaded it!

        // For now we are going to load coppers to a KiCad zone however this isn't perfect
        //TODO: Load onto a graphical polygon with a net (when KiCad has this feature)

        if( !mDoneCopperWarning )
        {
            wxLogWarning(
                    _( "The CADSTAR design contains COPPER elements, which have no direct KiCad "
                       "equivalent. These have been imported as a KiCad Zone if solid or hatch "
                       "filled, or as a KiCad Track if the shape was an unfilled outline (open or "
                       "closed)." ) );
            mDoneCopperWarning = true;
        }


        if( csCopper.Shape.Type == SHAPE_TYPE::OPENSHAPE
                || csCopper.Shape.Type == SHAPE_TYPE::OUTLINE )
        {
            std::vector<DRAWSEGMENT*> outlineSegments =
                    getDrawSegmentsFromVertices( csCopper.Shape.Vertices );

            std::vector<TRACK*> outlineTracks = makeTracksFromDrawsegments( outlineSegments, mBoard,
                    getKiCadNet(csCopper.NetRef.NetID) , getKiCadLayer( csCopper.LayerID ),
                    getKiCadLength( getCopperCode( csCopper.CopperCodeID ).CopperWidth ) );

            //cleanup
            for( DRAWSEGMENT* ds : outlineSegments )
            {
                if( ds )
                    delete ds;
            }

            for( CUTOUT cutout : csCopper.Shape.Cutouts )
            {
                std::vector<DRAWSEGMENT*> cutoutSeg =
                        getDrawSegmentsFromVertices( cutout.Vertices );

                std::vector<TRACK*> cutoutTracks = makeTracksFromDrawsegments( cutoutSeg, mBoard,
                        getKiCadNet( csCopper.NetRef.NetID ), getKiCadLayer( csCopper.LayerID ),
                        getKiCadLength( getCopperCode( csCopper.CopperCodeID ).CopperWidth ) );

                //cleanup
                for( DRAWSEGMENT* ds : cutoutSeg )
                {
                    if( ds )
                        delete ds;
                }
            }
        }
        else
        {
            ZONE_CONTAINER* zone = getZoneFromCadstarShape( csCopper.Shape,
                    getKiCadLength( getCopperCode( csCopper.CopperCodeID ).CopperWidth ), mBoard );

            mBoard->Add( zone, ADD_MODE::APPEND );

            zone->SetZoneName( csCopper.ID );
            zone->SetLayer( getKiCadLayer( csCopper.LayerID ) );

            if( csCopper.Shape.Type == SHAPE_TYPE::HATCHED )
            {
                zone->SetFillMode( ZONE_FILL_MODE::HATCH_PATTERN );
                zone->SetHatchGap( getKiCadHatchCodeGap( csCopper.Shape.HatchCodeID ) );
                zone->SetHatchThickness( getKiCadHatchCodeThickness( csCopper.Shape.HatchCodeID ) );
                zone->SetHatchOrientation( getHatchCodeAngleDegrees( csCopper.Shape.HatchCodeID ) );
            }
            else
            {
                zone->SetFillMode( ZONE_FILL_MODE::POLYGONS );
            }

            zone->SetPadConnection( ZONE_CONNECTION::FULL );
            zone->SetNet( getKiCadNet( csCopper.NetRef.NetID ) );
        }
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadNets()
{
    for( std::pair<NET_ID, NET> netPair : Layout.Nets )
    {
        NET net = netPair.second;
        wxString netnameForErrorReporting = net.Name;

        if( netnameForErrorReporting.IsEmpty() )
            netnameForErrorReporting = wxString::Format( "$%ld", net.SignalNum );

        for( NET::CONNECTION connection : net.Connections )
        {
            if( !connection.Unrouted )
                loadNetTracks( net.ID, connection.Route );

            //TODO: all other elements
        }

        for( std::pair<NETELEMENT_ID, NET::VIA> viaPair : net.Vias )
        {
            NET::VIA via = viaPair.second;
            loadNetVia( net.ID, via );
        }

        for( std::pair<NETELEMENT_ID, NET::PIN> pinPair : net.Pins )
        {
            NET::PIN pin = pinPair.second;
            MODULE*  m   = getModuleFromCadstarID( pin.ComponentID );

            if( m == nullptr )
            {
                wxLogWarning( wxString::Format(
                        _( "The net '%s' references component ID '%s' which does not exist. "
                           "This has been ignored," ),
                        netnameForErrorReporting, pin.ComponentID ) );
            }
            else if( ( pin.PadID - (long) 1 ) > m->Pads().size() )
            {
                wxLogWarning( wxString::Format(
                        _( "The net '%s' references non-existent pad index '%d' in component '%s'. "
                           "This has been ignored." ),
                        netnameForErrorReporting, pin.PadID, m->GetReference() ) );
            }
            else
            {
                // The below works because we have added the pads in the correct order to the module and
                // it so happens that PAD_ID in Cadstar is a sequential, numerical ID
                m->Pads().at( pin.PadID - (long) 1 )->SetNet( getKiCadNet( net.ID ) );
            }
        }
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadComponentAttributes(
        const COMPONENT& aComponent, MODULE* aModule )
{
    for( std::pair<ATTRIBUTE_ID, ATTRIBUTE_VALUE> attrPair : aComponent.AttributeValues )
    {
        ATTRIBUTE_VALUE& attrval = attrPair.second;

        if( attrval.HasLocation ) //only import attributes with location. Ignore the rest
            addAttribute( attrval.AttributeLocation, attrval.AttributeID, aModule, attrval.Value );
    }

    for( std::pair<ATTRIBUTE_ID, TEXT_LOCATION> textlocPair : aComponent.TextLocations )
    {
        TEXT_LOCATION& textloc = textlocPair.second;
        wxString       attrval;

        if( textloc.AttributeID == COMPONENT_NAME_ATTRID )
        {
            attrval = wxEmptyString; // Designator is loaded separately
        }
        else if( textloc.AttributeID == COMPONENT_NAME_2_ATTRID )
        {
            attrval = wxT( "${REFERENCE}" );
        }
        else if( textloc.AttributeID == PART_NAME_ATTRID )
        {
            attrval = getPart( aComponent.PartID ).Name;
        }
        else
            attrval = getAttributeValue( textloc.AttributeID, aComponent.AttributeValues );

        addAttribute( textloc, textloc.AttributeID, aModule, attrval );
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadNetTracks(
        const NET_ID& aCadstarNetID, const NET::ROUTE& aCadstarRoute )
{
    std::vector<DRAWSEGMENT*> dsVector;

    POINT prevEnd = aCadstarRoute.StartPoint;

    for( const NET::ROUTE_VERTEX& v : aCadstarRoute.RouteVertices )
    {
        DRAWSEGMENT* ds = getDrawSegmentFromVertex( prevEnd, v.Vertex );
        ds->SetLayer( getKiCadLayer( aCadstarRoute.LayerID ) );
        ds->SetWidth( getKiCadLength( v.RouteWidth ) );
        dsVector.push_back( ds );
        prevEnd = v.Vertex.End;
    }

    //Todo add real netcode to the tracks
    std::vector<TRACK*> tracks =
            makeTracksFromDrawsegments( dsVector, mBoard, getKiCadNet( aCadstarNetID ) );

    //cleanup
    for( DRAWSEGMENT* ds : dsVector )
    {
        if( ds )
            delete ds;
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::loadNetVia(
        const NET_ID& aCadstarNetID, const NET::VIA& aCadstarVia )
{
    VIA* via = new VIA( mBoard );
    mBoard->Add( via, ADD_MODE::APPEND );

    VIACODE   csViaCode   = getViaCode( aCadstarVia.ViaCodeID );
    LAYERPAIR csLayerPair = getLayerPair( aCadstarVia.LayerPairID );

    via->SetPosition( getKiCadPoint( aCadstarVia.Location ) );
    via->SetDrill( getKiCadLength( csViaCode.DrillDiameter ) );
    via->SetLocked( aCadstarVia.Fixed );

    if( csViaCode.Shape.ShapeType != PAD_SHAPE_TYPE::CIRCLE )
        wxLogError( wxString::Format(
                _( "The CADSTAR via code '%s' has different shape from a circle defined. "
                   "KiCad only supports circular vias so this via type has been changed to "
                   "be a via with circular shape of %.2f mm diameter." ),
                csViaCode.Name,
                (double) ( (double) getKiCadLength( csViaCode.Shape.Size ) / 1E6 ) ) );

    via->SetWidth( getKiCadLength( csViaCode.Shape.Size ) );

    bool start_layer_outside =
            csLayerPair.PhysicalLayerStart == 1
            || csLayerPair.PhysicalLayerStart == Assignments.Technology.MaxPhysicalLayer;
    bool end_layer_outside =
            csLayerPair.PhysicalLayerEnd == 1
            || csLayerPair.PhysicalLayerEnd == Assignments.Technology.MaxPhysicalLayer;

    if( start_layer_outside && end_layer_outside )
    {
        via->SetViaType( VIATYPE::THROUGH );
    }
    else if( ( !start_layer_outside ) && ( !end_layer_outside ) )
    {
        via->SetViaType( VIATYPE::BLIND_BURIED );
    }
    else
    {
        via->SetViaType( VIATYPE::MICROVIA );
    }

    via->SetLayerPair( getKiCadCopperLayerID( csLayerPair.PhysicalLayerStart ),
            getKiCadCopperLayerID( csLayerPair.PhysicalLayerEnd ) );
    via->SetNet( getKiCadNet( aCadstarNetID ) );
    ///todo add netcode to the via
}


void CADSTAR_PCB_ARCHIVE_LOADER::drawCadstarText( const TEXT& aCadstarText,
        BOARD_ITEM_CONTAINER* aContainer, const GROUP_ID& aCadstarGroupID,
        const LAYER_ID& aCadstarLayerOverride, const wxPoint& aMoveVector,
        const double& aRotationAngle, const double& aScalingFactor, const wxPoint& aTransformCentre,
        const bool& aMirrorInvert )
{
    TEXTE_PCB* txt = new TEXTE_PCB( aContainer );
    aContainer->Add( txt );
    txt->SetText( aCadstarText.Text );

    wxPoint rotatedTextPos = getKiCadPoint( aCadstarText.Position );
    RotatePoint( &rotatedTextPos, aTransformCentre, aRotationAngle );
    rotatedTextPos.x =
            KiROUND( (double) ( rotatedTextPos.x - aTransformCentre.x ) * aScalingFactor );
    rotatedTextPos.y =
            KiROUND( (double) ( rotatedTextPos.y - aTransformCentre.y ) * aScalingFactor );
    rotatedTextPos += aTransformCentre;
    txt->SetTextPos( rotatedTextPos );
    txt->SetPosition( rotatedTextPos );

    txt->SetTextAngle( getAngleTenthDegree( aCadstarText.OrientAngle ) + aRotationAngle );
    txt->SetMirrored( aCadstarText.Mirror );

    TEXTCODE tc = getTextCode( aCadstarText.TextCodeID );

    txt->SetTextThickness( getKiCadLength( tc.LineWidth ) );

    wxSize unscaledTextSize;
    unscaledTextSize.x = getKiCadLength( tc.Width );
    unscaledTextSize.y = getKiCadLength( tc.Height );
    txt->SetTextSize( unscaledTextSize );

    switch( aCadstarText.Alignment )
    {
    case ALIGNMENT::NO_ALIGNMENT: // Default for Single line text is Bottom Left
    case ALIGNMENT::BOTTOMLEFT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_BOTTOM );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_LEFT );
        break;

    case ALIGNMENT::BOTTOMCENTER:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_BOTTOM );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_CENTER );
        break;

    case ALIGNMENT::BOTTOMRIGHT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_BOTTOM );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_RIGHT );
        break;

    case ALIGNMENT::CENTERLEFT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_CENTER );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_LEFT );
        break;

    case ALIGNMENT::CENTERCENTER:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_CENTER );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_CENTER );
        break;

    case ALIGNMENT::CENTERRIGHT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_CENTER );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_RIGHT );
        break;

    case ALIGNMENT::TOPLEFT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_TOP );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_LEFT );
        break;

    case ALIGNMENT::TOPCENTER:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_TOP );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_CENTER );
        break;

    case ALIGNMENT::TOPRIGHT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_TOP );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_RIGHT );
        break;

    default:
        wxASSERT_MSG( true, "Unknown Aligment - needs review!" );
    }

    if( aMirrorInvert )
    {
        txt->Flip( aTransformCentre, true );
    }

    //scale it after flipping:
    if( aScalingFactor != 1.0 )
    {
        wxSize scaledTextSize;
        scaledTextSize.x = KiROUND( (double) getKiCadLength( tc.Width ) * aScalingFactor );
        scaledTextSize.y = KiROUND( (double) getKiCadLength( tc.Height ) * aScalingFactor );
        txt->SetTextSize( scaledTextSize );
        txt->SetTextThickness(
                KiROUND( (double) getKiCadLength( tc.LineWidth ) * aScalingFactor ) );
    }

    txt->Move( aMoveVector );

    LAYER_ID layersToDrawOn = aCadstarLayerOverride;

    if( layersToDrawOn.IsEmpty() )
        layersToDrawOn = aCadstarText.LayerID;

    if( isLayerSet( layersToDrawOn ) )
    {
        //Make a copy on each layer

        LSEQ       layers = getKiCadLayerSet( layersToDrawOn ).Seq();
        TEXTE_PCB* newtxt;

        for( PCB_LAYER_ID layer : layers )
        {
            txt->SetLayer( layer );
            newtxt = new TEXTE_PCB( *txt );
            mBoard->Add( newtxt, ADD_MODE::APPEND );

            if( !aCadstarGroupID.IsEmpty() )
                addToGroup( aCadstarGroupID, newtxt );
        }

        mBoard->Remove( txt );
        delete txt;
    }
    else
    {
        txt->SetLayer( getKiCadLayer( layersToDrawOn ) );

        if( !aCadstarGroupID.IsEmpty() )
            addToGroup( aCadstarGroupID, txt );
    }
    //TODO Handle different font types when KiCad can support it.
}


void CADSTAR_PCB_ARCHIVE_LOADER::drawCadstarShape( const SHAPE& aCadstarShape,
        const PCB_LAYER_ID& aKiCadLayer, const int& aLineThickness, const wxString& aShapeName,
        BOARD_ITEM_CONTAINER* aContainer, const GROUP_ID& aCadstarGroupID,
        const wxPoint& aMoveVector, const double& aRotationAngle, const double& aScalingFactor,
        const wxPoint& aTransformCentre, const bool& aMirrorInvert )
{
    switch( aCadstarShape.Type )
    {
    case SHAPE_TYPE::OPENSHAPE:
    case SHAPE_TYPE::OUTLINE:
        ///TODO update this when Polygons in KiCad can be defined with no fill
        drawCadstarVerticesAsSegments( aCadstarShape.Vertices, aKiCadLayer, aLineThickness,
                aContainer, aCadstarGroupID, aMoveVector, aRotationAngle, aScalingFactor,
                aTransformCentre, aMirrorInvert );
        drawCadstarCutoutsAsSegments( aCadstarShape.Cutouts, aKiCadLayer, aLineThickness,
                aContainer, aCadstarGroupID, aMoveVector, aRotationAngle, aScalingFactor,
                aTransformCentre, aMirrorInvert );
        break;

    case SHAPE_TYPE::HATCHED:
        ///TODO update this when Polygons in KiCad can be defined with hatch fill
        wxLogWarning( wxString::Format(
                _( "The shape for '%s' is Hatch filled in CADSTAR, which has no KiCad equivalent. "
                   "Using solid fill instead." ),
                aShapeName ) );

    case SHAPE_TYPE::SOLID:
    {
        DRAWSEGMENT* ds;

        if( isModule( aContainer ) )
            ds = new EDGE_MODULE( (MODULE*) aContainer, STROKE_T::S_POLYGON );
        else
        {
            ds = new DRAWSEGMENT( aContainer );
            ds->SetShape( STROKE_T::S_POLYGON );
        }

        ds->SetPolyShape( getPolySetFromCadstarShape( aCadstarShape, -1, aContainer, aMoveVector,
                aRotationAngle, aScalingFactor, aTransformCentre, aMirrorInvert ) );
        ds->SetWidth( aLineThickness );
        ds->SetLayer( aKiCadLayer );
        aContainer->Add( ds, ADD_MODE::APPEND );

        if( !aCadstarGroupID.IsEmpty() )
            addToGroup( aCadstarGroupID, ds );
    }
    break;
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::drawCadstarCutoutsAsSegments( const std::vector<CUTOUT>& aCutouts,
        const PCB_LAYER_ID& aKiCadLayer, const int& aLineThickness,
        BOARD_ITEM_CONTAINER* aContainer, const GROUP_ID& aCadstarGroupID,
        const wxPoint& aMoveVector, const double& aRotationAngle, const double& aScalingFactor,
        const wxPoint& aTransformCentre, const bool& aMirrorInvert )
{
    for( CUTOUT cutout : aCutouts )
    {
        drawCadstarVerticesAsSegments( cutout.Vertices, aKiCadLayer, aLineThickness, aContainer,
                aCadstarGroupID, aMoveVector, aRotationAngle, aScalingFactor, aTransformCentre,
                aMirrorInvert );
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::drawCadstarVerticesAsSegments(
        const std::vector<VERTEX>& aCadstarVertices, const PCB_LAYER_ID& aKiCadLayer,
        const int& aLineThickness, BOARD_ITEM_CONTAINER* aContainer,
        const GROUP_ID& aCadstarGroupID, const wxPoint& aMoveVector, const double& aRotationAngle,
        const double& aScalingFactor, const wxPoint& aTransformCentre, const bool& aMirrorInvert )
{
    std::vector<DRAWSEGMENT*> drawSegments =
            getDrawSegmentsFromVertices( aCadstarVertices, aContainer, aCadstarGroupID, aMoveVector,
                    aRotationAngle, aScalingFactor, aTransformCentre, aMirrorInvert );

    for( DRAWSEGMENT* ds : drawSegments )
    {
        ds->SetWidth( aLineThickness );
        ds->SetLayer( aKiCadLayer );
        ds->SetParent( aContainer );
        aContainer->Add( ds, ADD_MODE::APPEND );
    }
}


std::vector<DRAWSEGMENT*> CADSTAR_PCB_ARCHIVE_LOADER::getDrawSegmentsFromVertices(
        const std::vector<VERTEX>& aCadstarVertices, BOARD_ITEM_CONTAINER* aContainer,
        const GROUP_ID& aCadstarGroupID, const wxPoint& aMoveVector, const double& aRotationAngle,
        const double& aScalingFactor, const wxPoint& aTransformCentre, const bool& aMirrorInvert )
{
    std::vector<DRAWSEGMENT*> drawSegments;

    if( aCadstarVertices.size() < 2 )
        //need at least two points to draw a segment! (unlikely but possible to have only one)
        return drawSegments;

    const VERTEX* prev = &aCadstarVertices.at( 0 ); // first one should always be a point vertex
    const VERTEX* cur;

    for( size_t i = 1; i < aCadstarVertices.size(); i++ )
    {
        cur = &aCadstarVertices.at( i );
        drawSegments.push_back(
                getDrawSegmentFromVertex( prev->End, *cur, aContainer, aCadstarGroupID, aMoveVector,
                        aRotationAngle, aScalingFactor, aTransformCentre, aMirrorInvert ) );
        prev = cur;
    }

    return drawSegments;
}


DRAWSEGMENT* CADSTAR_PCB_ARCHIVE_LOADER::getDrawSegmentFromVertex( const POINT& aCadstarStartPoint,
        const VERTEX& aCadstarVertex, BOARD_ITEM_CONTAINER* aContainer,
        const GROUP_ID& aCadstarGroupID, const wxPoint& aMoveVector, const double& aRotationAngle,
        const double& aScalingFactor, const wxPoint& aTransformCentre, const bool& aMirrorInvert )
{
    DRAWSEGMENT* ds = nullptr;
    bool         cw = false;
    double       arcStartAngle, arcEndAngle, arcAngle;

    wxPoint startPoint = getKiCadPoint( aCadstarStartPoint );
    wxPoint endPoint   = getKiCadPoint( aCadstarVertex.End );
    wxPoint centerPoint;

    if( aCadstarVertex.Type == VERTEX_TYPE::ANTICLOCKWISE_SEMICIRCLE
            || aCadstarVertex.Type == VERTEX_TYPE::CLOCKWISE_SEMICIRCLE )
        centerPoint = ( startPoint + endPoint ) / 2;
    else
        centerPoint = getKiCadPoint( aCadstarVertex.Center );

    switch( aCadstarVertex.Type )
    {

    case VERTEX_TYPE::POINT:

        if( isModule( aContainer ) )
        {
            ds = new EDGE_MODULE( static_cast<MODULE*>( aContainer ), STROKE_T::S_SEGMENT );
        }
        else
        {
            ds = new DRAWSEGMENT( aContainer );
            ds->SetShape( STROKE_T::S_SEGMENT );
        }

        ds->SetStart( startPoint );
        ds->SetEnd( endPoint );
        break;

    case VERTEX_TYPE::CLOCKWISE_SEMICIRCLE:
    case VERTEX_TYPE::CLOCKWISE_ARC:
        cw = true;
        KI_FALLTHROUGH;

    case VERTEX_TYPE::ANTICLOCKWISE_SEMICIRCLE:
    case VERTEX_TYPE::ANTICLOCKWISE_ARC:

        if( isModule( aContainer ) )
        {
            ds = new EDGE_MODULE( (MODULE*) aContainer, STROKE_T::S_ARC );
        }
        else
        {
            ds = new DRAWSEGMENT( aContainer );
            ds->SetShape( STROKE_T::S_ARC );
        }

        ds->SetArcStart( startPoint );
        ds->SetCenter( centerPoint );

        arcStartAngle = getPolarAngle( startPoint - centerPoint );
        arcEndAngle   = getPolarAngle( endPoint - centerPoint );
        arcAngle      = arcEndAngle - arcStartAngle;
        //TODO: detect if we are supposed to draw a circle instead (i.e. two SEMICIRCLEs
        // with opposite start/end points and same centre point)

        if( cw )
            ds->SetAngle( NormalizeAnglePos( arcAngle ) );
        else
            ds->SetAngle( NormalizeAngleNeg( arcAngle ) );

        break;
    }

    //Apply transforms
    if( aMirrorInvert )
        ds->Flip( aTransformCentre, true );

    if( aScalingFactor != 1.0 )
    {
        ds->Move( -aTransformCentre );
        ds->Scale( aScalingFactor );
        ds->Move( aTransformCentre );
    }

    if( aRotationAngle != 0.0 )
        ds->Rotate( aTransformCentre, aRotationAngle );

    if( aMoveVector != wxPoint{ 0, 0 } )
        ds->Move( aMoveVector );

    if( isModule( aContainer ) && ds != nullptr )
        static_cast<EDGE_MODULE*>( ds )->SetLocalCoord();

    if( !aCadstarGroupID.IsEmpty() )
        addToGroup( aCadstarGroupID, ds );

    return ds;
}


ZONE_CONTAINER* CADSTAR_PCB_ARCHIVE_LOADER::getZoneFromCadstarShape(
        const SHAPE& aCadstarShape, const int& aLineThickness, BOARD_ITEM_CONTAINER* aParentContainer )
{
    ZONE_CONTAINER* zone = new ZONE_CONTAINER( aParentContainer, isModule( aParentContainer ) );

    if( aCadstarShape.Type == SHAPE_TYPE::HATCHED )
    {
        zone->SetFillMode( ZONE_FILL_MODE::HATCH_PATTERN );
        zone->SetHatchStyle( ZONE_BORDER_DISPLAY_STYLE::DIAGONAL_FULL );
    }
    else
    {
        zone->SetHatchStyle( ZONE_BORDER_DISPLAY_STYLE::NO_HATCH );
    }

    SHAPE_POLY_SET polygon = getPolySetFromCadstarShape( aCadstarShape, aLineThickness );

    zone->AddPolygon( polygon.COutline( 0 ) );

    for( int i = 0; i < polygon.HoleCount( 0 ); i++ )
        zone->AddPolygon( polygon.CHole( 0, i ) );

    return zone;
}


SHAPE_POLY_SET CADSTAR_PCB_ARCHIVE_LOADER::getPolySetFromCadstarShape( const SHAPE& aCadstarShape,
        const int& aLineThickness, BOARD_ITEM_CONTAINER* aContainer, const wxPoint& aMoveVector,
        const double& aRotationAngle, const double& aScalingFactor, const wxPoint& aTransformCentre,
        const bool& aMirrorInvert )
{
    GROUP_ID noGroup = wxEmptyString;

    std::vector<DRAWSEGMENT*> outlineSegments =
            getDrawSegmentsFromVertices( aCadstarShape.Vertices, aContainer, noGroup, aMoveVector,
                    aRotationAngle, aScalingFactor, aTransformCentre, aMirrorInvert );

    SHAPE_POLY_SET polySet( getLineChainFromDrawsegments( outlineSegments ) );

    //cleanup
    for( DRAWSEGMENT* ds : outlineSegments )
    {
        if( ds )
            delete ds;
    }

    for( CUTOUT cutout : aCadstarShape.Cutouts )
    {
        std::vector<DRAWSEGMENT*> cutoutSeg =
                getDrawSegmentsFromVertices( cutout.Vertices, aContainer, noGroup, aMoveVector,
                        aRotationAngle, aScalingFactor, aTransformCentre, aMirrorInvert );

        polySet.AddHole( getLineChainFromDrawsegments( cutoutSeg ) );

        //cleanup
        for( DRAWSEGMENT* ds : cutoutSeg )
        {
            if( ds )
                delete ds;
        }
    }

    if( aLineThickness > 0 )
        polySet.Inflate(
                aLineThickness / 2, 32, SHAPE_POLY_SET::CORNER_STRATEGY::ROUND_ALL_CORNERS );

    //Make a new polyset with no holes
    //TODO: Using strictly simple to be safe, but need to find out if PM_FAST works okay
    polySet.Fracture( SHAPE_POLY_SET::POLYGON_MODE::PM_STRICTLY_SIMPLE );

#ifdef DEBUG
    for( int i = 0; i < polySet.OutlineCount(); ++i )
    {
        wxASSERT( polySet.Outline( i ).PointCount() > 2 );

        for( int j = 0; j < polySet.HoleCount( i ); ++j )
        {
            wxASSERT( polySet.Hole( i, j ).PointCount() > 2 );
        }
    }
#endif

    return polySet;
}


SHAPE_LINE_CHAIN CADSTAR_PCB_ARCHIVE_LOADER::getLineChainFromDrawsegments(
        const std::vector<DRAWSEGMENT*> aDrawsegments )
{
    SHAPE_LINE_CHAIN lineChain;

    for( DRAWSEGMENT* ds : aDrawsegments )
    {
        switch( ds->GetShape() )
        {
        case STROKE_T::S_ARC:
        {
            if( ds->GetClass() == wxT( "MGRAPHIC" ) )
            {
                EDGE_MODULE* em = (EDGE_MODULE*) ds;
                SHAPE_ARC    arc( em->GetStart0(), em->GetEnd0(), (double) em->GetAngle() / 10.0 );
                lineChain.Append( arc );
            }
            else
            {
                SHAPE_ARC arc( ds->GetCenter(), ds->GetArcStart(), (double) ds->GetAngle() / 10.0 );
                lineChain.Append( arc );
            }
        }
        break;
        case STROKE_T::S_SEGMENT:
            if( ds->GetClass() == wxT( "MGRAPHIC" ) )
            {
                EDGE_MODULE* em = (EDGE_MODULE*) ds;
                lineChain.Append( em->GetStart0().x, em->GetStart0().y );
                lineChain.Append( em->GetEnd0().x, em->GetEnd0().y );
            }
            else
            {
                lineChain.Append( ds->GetStartX(), ds->GetStartY() );
                lineChain.Append( ds->GetEndX(), ds->GetEndY() );
            }
            break;

        default:
            wxASSERT_MSG( true, "Drawsegment type is unexpected. Ignored." );
        }
    }

    lineChain.SetClosed( true ); //todo check if it is closed

    wxASSERT( lineChain.PointCount() > 2 );

    return lineChain;
}


std::vector<TRACK*> CADSTAR_PCB_ARCHIVE_LOADER::makeTracksFromDrawsegments(
        const std::vector<DRAWSEGMENT*> aDrawsegments, BOARD_ITEM_CONTAINER* aParentContainer,
        NETINFO_ITEM* aNet, const PCB_LAYER_ID& aLayerOverride, int aWidthOverride )
{
    std::vector<TRACK*> tracks;

    for( DRAWSEGMENT* ds : aDrawsegments )
    {
        TRACK* track;

        switch( ds->GetShape() )
        {
        case STROKE_T::S_ARC:
            if( ds->GetClass() == wxT( "MGRAPHIC" ) )
            {
                EDGE_MODULE* em = (EDGE_MODULE*) ds;
                SHAPE_ARC    arc( em->GetStart0(), em->GetEnd0(), (double) em->GetAngle() / 10.0 );
                track = new ARC( aParentContainer, &arc );
            }
            else
            {
                SHAPE_ARC arc( ds->GetCenter(), ds->GetArcStart(), (double) ds->GetAngle() / 10.0 );
                track = new ARC( aParentContainer, &arc );
            }
            break;
        case STROKE_T::S_SEGMENT:
            if( ds->GetClass() == wxT( "MGRAPHIC" ) )
            {
                EDGE_MODULE* em = (EDGE_MODULE*) ds;
                track           = new TRACK( aParentContainer );
                track->SetStart( em->GetStart0() );
                track->SetEnd( em->GetEnd0() );
            }
            else
            {
                track = new TRACK( aParentContainer );
                track->SetStart( ds->GetStart() );
                track->SetEnd( ds->GetEnd() );
            }
            break;

        default:
            wxASSERT_MSG( true, "Drawsegment type is unexpected. Ignored." );
            continue;
        }

        if( aWidthOverride == -1 )
            track->SetWidth( ds->GetWidth() );
        else
            track->SetWidth( aWidthOverride );

        if( aLayerOverride == PCB_LAYER_ID::UNDEFINED_LAYER )
            track->SetLayer( ds->GetLayer() );
        else
            track->SetLayer( aLayerOverride );

        if( aNet != nullptr )
            track->SetNet( aNet );

        tracks.push_back( track );
        aParentContainer->Add( track, ADD_MODE::APPEND );
    }

    return tracks;
}


void CADSTAR_PCB_ARCHIVE_LOADER::addAttribute( const ATTRIBUTE_LOCATION& aCadstarAttrLoc,
        const ATTRIBUTE_ID& aCadstarAttributeID, MODULE* aModule, const wxString& aAttributeValue )
{
    TEXTE_MODULE* txt;

    if( aCadstarAttributeID == COMPONENT_NAME_ATTRID )
        txt = &aModule->Reference(); //text should be set outside this function
    else if( aCadstarAttributeID == PART_NAME_ATTRID )
    {
        if( aModule->Value().GetText().IsEmpty() )
        {
            // Use PART_NAME_ATTRID as the value is value field is blank
            aModule->SetValue( aAttributeValue );
            txt = &aModule->Value();
        }
        else
        {
            txt = new TEXTE_MODULE( aModule );
            aModule->Add( txt );
            txt->SetText( aAttributeValue );
        }
        txt->SetVisible( false ); //make invisible to avoid clutter.
    }
    else if( aCadstarAttributeID != COMPONENT_NAME_2_ATTRID
             && getAttributeName( aCadstarAttributeID ) == wxT( "Value" ) )
    {
        if( !aModule->Value().GetText().IsEmpty() )
        {
            //copy the object
            aModule->Add( new TEXTE_MODULE( aModule->Value() ) );
        }

        aModule->SetValue( aAttributeValue );
        txt = &aModule->Value();
        txt->SetVisible( false ); //make invisible to avoid clutter.
    }
    else
    {
        txt = new TEXTE_MODULE( aModule );
        aModule->Add( txt );
        txt->SetText( aAttributeValue );
        txt->SetVisible( false ); //make all user attributes invisible to avoid clutter.
        //TODO: Future improvement - allow user to decide what to do with attributes
    }

    wxPoint rotatedTextPos = getKiCadPoint( aCadstarAttrLoc.Position ) - aModule->GetPosition();
    RotatePoint( &rotatedTextPos, -aModule->GetOrientation() );

    txt->SetTextPos( getKiCadPoint( aCadstarAttrLoc.Position ) );
    txt->SetPos0( rotatedTextPos );
    txt->SetLayer( getKiCadLayer( aCadstarAttrLoc.LayerID ) );
    txt->SetMirrored( aCadstarAttrLoc.Mirror );
    txt->SetTextAngle( getAngleTenthDegree( aCadstarAttrLoc.OrientAngle ) - aModule->GetOrientation() );

    TEXTCODE tc = getTextCode( aCadstarAttrLoc.TextCodeID );

    txt->SetTextThickness( getKiCadLength( tc.LineWidth ) );
    txt->SetTextSize( { getKiCadLength( tc.Width ), getKiCadLength( tc.Height ) } );

    switch( aCadstarAttrLoc.Alignment )
    {
    case ALIGNMENT::NO_ALIGNMENT: // Default for Single line text is Bottom Left
    case ALIGNMENT::BOTTOMLEFT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_BOTTOM );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_LEFT );
        break;

    case ALIGNMENT::BOTTOMCENTER:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_BOTTOM );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_CENTER );
        break;

    case ALIGNMENT::BOTTOMRIGHT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_BOTTOM );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_RIGHT );
        break;

    case ALIGNMENT::CENTERLEFT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_CENTER );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_LEFT );
        break;

    case ALIGNMENT::CENTERCENTER:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_CENTER );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_CENTER );
        break;

    case ALIGNMENT::CENTERRIGHT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_CENTER );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_RIGHT );
        break;

    case ALIGNMENT::TOPLEFT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_TOP );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_LEFT );
        break;

    case ALIGNMENT::TOPCENTER:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_TOP );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_CENTER );
        break;

    case ALIGNMENT::TOPRIGHT:
        txt->SetVertJustify( GR_TEXT_VJUSTIFY_TOP );
        txt->SetHorizJustify( GR_TEXT_HJUSTIFY_RIGHT );
        break;

    default:
        wxASSERT_MSG( true, "Unknown Aligment - needs review!" );
    }

    //TODO Handle different font types when KiCad can support it.
}


int CADSTAR_PCB_ARCHIVE_LOADER::getLineThickness( const LINECODE_ID& aCadstarLineCodeID )
{
    wxCHECK( Assignments.Codedefs.LineCodes.find( aCadstarLineCodeID )
                     != Assignments.Codedefs.LineCodes.end(),
            mBoard->GetDesignSettings().GetLineThickness( PCB_LAYER_ID::Edge_Cuts ) );

    return getKiCadLength( Assignments.Codedefs.LineCodes.at( aCadstarLineCodeID ).Width );
}


CADSTAR_PCB_ARCHIVE_LOADER::COPPERCODE CADSTAR_PCB_ARCHIVE_LOADER::getCopperCode(
        const COPPERCODE_ID& aCadstaCopperCodeID )
{
    wxCHECK( Assignments.Codedefs.CopperCodes.find( aCadstaCopperCodeID )
                     != Assignments.Codedefs.CopperCodes.end(),
            COPPERCODE() );

    return Assignments.Codedefs.CopperCodes.at( aCadstaCopperCodeID );
}


CADSTAR_PCB_ARCHIVE_LOADER::TEXTCODE CADSTAR_PCB_ARCHIVE_LOADER::getTextCode(
        const TEXTCODE_ID& aCadstarTextCodeID )
{
    wxCHECK( Assignments.Codedefs.TextCodes.find( aCadstarTextCodeID )
                     != Assignments.Codedefs.TextCodes.end(),
            TEXTCODE() );

    return Assignments.Codedefs.TextCodes.at( aCadstarTextCodeID );
}


CADSTAR_PCB_ARCHIVE_LOADER::PADCODE CADSTAR_PCB_ARCHIVE_LOADER::getPadCode(
        const PADCODE_ID& aCadstarPadCodeID )
{
    wxCHECK( Assignments.Codedefs.PadCodes.find( aCadstarPadCodeID )
                     != Assignments.Codedefs.PadCodes.end(),
            PADCODE() );

    return Assignments.Codedefs.PadCodes.at( aCadstarPadCodeID );
}


CADSTAR_PCB_ARCHIVE_LOADER::VIACODE CADSTAR_PCB_ARCHIVE_LOADER::getViaCode(
        const VIACODE_ID& aCadstarViaCodeID )
{
    wxCHECK( Assignments.Codedefs.ViaCodes.find( aCadstarViaCodeID )
                     != Assignments.Codedefs.ViaCodes.end(),
            VIACODE() );

    return Assignments.Codedefs.ViaCodes.at( aCadstarViaCodeID );
}


CADSTAR_PCB_ARCHIVE_LOADER::LAYERPAIR CADSTAR_PCB_ARCHIVE_LOADER::getLayerPair(
        const LAYERPAIR_ID& aCadstarLayerPairID )
{
    wxCHECK( Assignments.Codedefs.LayerPairs.find( aCadstarLayerPairID )
                     != Assignments.Codedefs.LayerPairs.end(),
            LAYERPAIR() );

    return Assignments.Codedefs.LayerPairs.at( aCadstarLayerPairID );
}


wxString CADSTAR_PCB_ARCHIVE_LOADER::getAttributeName( const ATTRIBUTE_ID& aCadstarAttributeID )
{
    wxCHECK( Assignments.Codedefs.AttributeNames.find( aCadstarAttributeID )
                     != Assignments.Codedefs.AttributeNames.end(),
            wxEmptyString );

    return Assignments.Codedefs.AttributeNames.at( aCadstarAttributeID ).Name;
}


wxString CADSTAR_PCB_ARCHIVE_LOADER::getAttributeValue( const ATTRIBUTE_ID& aCadstarAttributeID,
        const std::map<ATTRIBUTE_ID, ATTRIBUTE_VALUE>&                      aCadstarAttributeMap )
{
    wxCHECK( aCadstarAttributeMap.find( aCadstarAttributeID ) != aCadstarAttributeMap.end(),
            wxEmptyString );

    return aCadstarAttributeMap.at( aCadstarAttributeID ).Value;
}


CADSTAR_PCB_ARCHIVE_LOADER::PART CADSTAR_PCB_ARCHIVE_LOADER::getPart(
        const PART_ID& aCadstarPartID )
{
    wxCHECK( Parts.PartDefinitions.find( aCadstarPartID ) != Parts.PartDefinitions.end(), PART() );

    return Parts.PartDefinitions.at( aCadstarPartID );
}


CADSTAR_PCB_ARCHIVE_LOADER::ROUTECODE CADSTAR_PCB_ARCHIVE_LOADER::getRouteCode(
        const ROUTECODE_ID& aCadstarRouteCodeID )
{
    wxCHECK( Assignments.Codedefs.RouteCodes.find( aCadstarRouteCodeID )
                     != Assignments.Codedefs.RouteCodes.end(),
            ROUTECODE() );

    return Assignments.Codedefs.RouteCodes.at( aCadstarRouteCodeID );
}


CADSTAR_PCB_ARCHIVE_LOADER::HATCHCODE CADSTAR_PCB_ARCHIVE_LOADER::getHatchCode(
        const HATCHCODE_ID& aCadstarHatchcodeID )
{
    wxCHECK( Assignments.Codedefs.HatchCodes.find( aCadstarHatchcodeID )
                     != Assignments.Codedefs.HatchCodes.end(),
            HATCHCODE() );

    return Assignments.Codedefs.HatchCodes.at( aCadstarHatchcodeID );
}


double CADSTAR_PCB_ARCHIVE_LOADER::getHatchCodeAngleDegrees( const HATCHCODE_ID& aCadstarHatchcodeID )
{
    checkAndLogHatchCode( aCadstarHatchcodeID );
    HATCHCODE hcode = getHatchCode( aCadstarHatchcodeID );

    if( hcode.Hatches.size() < 1 )
        return mBoard->GetDesignSettings().GetDefaultZoneSettings().m_HatchOrientation;
    else
        return getAngleDegrees( hcode.Hatches.at( 0 ).OrientAngle );
}


int CADSTAR_PCB_ARCHIVE_LOADER::getKiCadHatchCodeThickness(
        const HATCHCODE_ID& aCadstarHatchcodeID )
{
    checkAndLogHatchCode( aCadstarHatchcodeID );
    HATCHCODE hcode = getHatchCode( aCadstarHatchcodeID );

    if( hcode.Hatches.size() < 1 )
        return mBoard->GetDesignSettings().GetDefaultZoneSettings().m_HatchThickness;
    else
        return getKiCadLength( hcode.Hatches.at( 0 ).LineWidth );
}


int CADSTAR_PCB_ARCHIVE_LOADER::getKiCadHatchCodeGap( const HATCHCODE_ID& aCadstarHatchcodeID )
{
    checkAndLogHatchCode( aCadstarHatchcodeID );
    HATCHCODE hcode = getHatchCode( aCadstarHatchcodeID );

    if( hcode.Hatches.size() < 1 )
        return mBoard->GetDesignSettings().GetDefaultZoneSettings().m_HatchGap;
    else
        return getKiCadLength( hcode.Hatches.at( 0 ).Step );
}


PCB_GROUP* CADSTAR_PCB_ARCHIVE_LOADER::getKiCadGroup( const GROUP_ID& aCadstarGroupID )
{
    wxCHECK( mGroupMap.find( aCadstarGroupID ) != mGroupMap.end(), nullptr );

    return mGroupMap.at( aCadstarGroupID );
}


void CADSTAR_PCB_ARCHIVE_LOADER::checkAndLogHatchCode( const HATCHCODE_ID& aCadstarHatchcodeID )
{
    if( mHatchcodesTested.find( aCadstarHatchcodeID ) != mHatchcodesTested.end() )
    {
        return; //already checked
    }
    else
    {
        HATCHCODE hcode = getHatchCode( aCadstarHatchcodeID );

        if( hcode.Hatches.size() != 2 )
        {
            wxLogWarning( wxString::Format(
                    _( "The CADSTAR Hatching code '%s' has %d hatches defined. "
                       "KiCad only supports 2 hatches (crosshatching) 90 degrees apart. "
                       "The imported hatching is crosshatched." ),
                    hcode.Name, (int) hcode.Hatches.size() ) );
        }
        else
        {
            if( hcode.Hatches.at( 0 ).LineWidth != hcode.Hatches.at( 1 ).LineWidth )
            {
                wxLogWarning( wxString::Format(
                        _( "The CADSTAR Hatching code '%s' has different line widths for each "
                           "hatch. KiCad only supports one width for the haching. The imported "
                           "hatching uses the width defined in the first hatch definition, i.e. "
                           "%.2f mm." ),
                        hcode.Name,
                        (double) ((double) getKiCadLength( hcode.Hatches.at( 0 ).LineWidth ) ) / 1E6 ) );
            }

            if( hcode.Hatches.at( 0 ).Step != hcode.Hatches.at( 1 ).Step )
            {
                wxLogWarning( wxString::Format(
                        _( "The CADSTAR Hatching code '%s' has different step sizes for each "
                           "hatch. KiCad only supports one step size for the haching. The imported "
                           "hatching uses the step size defined in the first hatching definition, "
                           "i.e. %.2f mm." ),
                        hcode.Name,
                        (double) ( (double) getKiCadLength( hcode.Hatches.at( 0 ).Step ) )
                                / 1E6 ) );
            }

            if( abs( hcode.Hatches.at( 0 ).OrientAngle - hcode.Hatches.at( 1 ).OrientAngle )
                    != 90000 )
            {
                wxLogWarning( wxString::Format(
                        _( "The hatches in CADSTAR Hatching code '%s' have an angle  "
                           "difference of %.1f degrees. KiCad only supports hatching 90 "
                           "degrees apart.  The imported hatching has two hatches 90 "
                           "degrees apart, oriented %.1f degrees from horizontal." ),
                        hcode.Name,
                        getAngleDegrees( abs( hcode.Hatches.at( 0 ).OrientAngle
                                              - hcode.Hatches.at( 1 ).OrientAngle ) ),
                        getAngleDegrees( hcode.Hatches.at( 0 ).OrientAngle ) ) );
            }
        }

        mHatchcodesTested.insert( aCadstarHatchcodeID );
    }
}


MODULE* CADSTAR_PCB_ARCHIVE_LOADER::getModuleFromCadstarID(
        const COMPONENT_ID& aCadstarComponentID )
{
    if( mComponentMap.find( aCadstarComponentID ) == mComponentMap.end() )
        return nullptr;
    else
        return mComponentMap.at( aCadstarComponentID );
}


wxPoint CADSTAR_PCB_ARCHIVE_LOADER::getKiCadPoint( wxPoint aCadstarPoint )
{
    wxPoint retval;

    retval.x = ( aCadstarPoint.x - mDesignCenter.x ) * KiCadUnitMultiplier;
    retval.y = -( aCadstarPoint.y - mDesignCenter.y ) * KiCadUnitMultiplier;

    return retval;
}


double CADSTAR_PCB_ARCHIVE_LOADER::getPolarAngle( wxPoint aPoint )
{

    return NormalizeAnglePos( ArcTangente( aPoint.y, aPoint.x ) );
}


NETINFO_ITEM* CADSTAR_PCB_ARCHIVE_LOADER::getKiCadNet( const NET_ID& aCadstarNetID )
{
    if( aCadstarNetID.IsEmpty() )
        return nullptr;
    else if( mNetMap.find( aCadstarNetID ) != mNetMap.end() )
    {
        return mNetMap.at(aCadstarNetID);
    }
    else
    {
        wxCHECK( Layout.Nets.find( aCadstarNetID ) != Layout.Nets.end(), nullptr );

        NET csNet = Layout.Nets.at( aCadstarNetID );
        wxString newName = csNet.Name;

        if( csNet.Name.IsEmpty() )
        {
            if( csNet.Pins.size() > 0 )
            {
                // Create default KiCad net naming:

                NET::PIN firstPin = (*csNet.Pins.begin()).second;
                //we should have already loaded the component with loadComponents() :
                MODULE* m = getModuleFromCadstarID( firstPin.ComponentID );
                newName   = wxT( "Net-(" );
                newName << m->Reference().GetText();
                newName << "-Pad" << wxString::Format( "%i", firstPin.PadID) << ")";
            }
            else
            {
                wxASSERT_MSG( false, "A net with no pins associated?" );
                newName = wxT( "csNet-" );
                newName << wxString::Format( "%i", csNet.SignalNum );
            }
        }

        if( !mDoneNetClassWarning && !csNet.NetClassID.IsEmpty()
                && csNet.NetClassID != wxT( "NONE" ) )
        {
            wxLogMessage( _(
                    "The CADSTAR design contains nets with a 'Net Class' assigned. KiCad does "
                    "not have an equivalent to CADSTAR's Net Class so these elements were not "
                    "imported. Note: KiCad's version of 'Net Class' is closer to CADSTAR's "
                    "'Net Route Code' (which has been imported for all nets)." ) );
            mDoneNetClassWarning = true;
        }

        if( !mDoneSpacingClassWarning && !csNet.SpacingClassID.IsEmpty()
                && csNet.SpacingClassID != wxT( "NONE" ) )
        {
            wxLogWarning( _(
                    "The CADSTAR design contains nets with a 'Spacing Class' assigned. KiCad does "
                    "not have an equivalent to CADSTAR's Spacing Class so these elements were not "
                    "imported. Please review the design rules as copper pours will affected by "
                    "this." ) );
            mDoneSpacingClassWarning = true;
        }

        NETINFO_ITEM* netInfo = new NETINFO_ITEM( mBoard, newName, ++mNumNets );
        mBoard->Add( netInfo, ADD_MODE::APPEND );

        if( mNetClassMap.find( csNet.RouteCodeID ) != mNetClassMap.end() )
        {
            NETCLASSPTR netclass = mNetClassMap.at( csNet.RouteCodeID );
            netInfo->SetClass( netclass );
        }
        else
        {
            ROUTECODE   rc = getRouteCode( csNet.RouteCodeID );
            NETCLASSPTR netclass( new ::NETCLASS( rc.Name ) );
            netclass->SetTrackWidth( getKiCadLength( rc.OptimalWidth ) );
            netInfo->SetClass( netclass );
            mNetClassMap.insert( { csNet.RouteCodeID, netclass } );
        }

        mNetMap.insert( { aCadstarNetID, netInfo } );
        return netInfo;
    }

    return nullptr;
}


PCB_LAYER_ID CADSTAR_PCB_ARCHIVE_LOADER::getKiCadCopperLayerID( unsigned int aLayerNum )
{
    if(aLayerNum == Assignments.Technology.MaxPhysicalLayer)
        return PCB_LAYER_ID::B_Cu;

    switch( aLayerNum )
    {
    case 1:   return PCB_LAYER_ID::F_Cu;
    case 2:   return PCB_LAYER_ID::In1_Cu;
    case 3:   return PCB_LAYER_ID::In2_Cu;
    case 4:   return PCB_LAYER_ID::In3_Cu;
    case 5:   return PCB_LAYER_ID::In4_Cu;
    case 6:   return PCB_LAYER_ID::In5_Cu;
    case 7:   return PCB_LAYER_ID::In6_Cu;
    case 8:   return PCB_LAYER_ID::In7_Cu;
    case 9:   return PCB_LAYER_ID::In8_Cu;
    case 10:  return PCB_LAYER_ID::In9_Cu;
    case 11:  return PCB_LAYER_ID::In10_Cu;
    case 12:  return PCB_LAYER_ID::In11_Cu;
    case 13:  return PCB_LAYER_ID::In12_Cu;
    case 14:  return PCB_LAYER_ID::In13_Cu;
    case 15:  return PCB_LAYER_ID::In14_Cu;
    case 16:  return PCB_LAYER_ID::In15_Cu;
    case 17:  return PCB_LAYER_ID::In16_Cu;
    case 18:  return PCB_LAYER_ID::In17_Cu;
    case 19:  return PCB_LAYER_ID::In18_Cu;
    case 20:  return PCB_LAYER_ID::In19_Cu;
    case 21:  return PCB_LAYER_ID::In20_Cu;
    case 22:  return PCB_LAYER_ID::In21_Cu;
    case 23:  return PCB_LAYER_ID::In22_Cu;
    case 24:  return PCB_LAYER_ID::In23_Cu;
    case 25:  return PCB_LAYER_ID::In24_Cu;
    case 26:  return PCB_LAYER_ID::In25_Cu;
    case 27:  return PCB_LAYER_ID::In26_Cu;
    case 28:  return PCB_LAYER_ID::In27_Cu;
    case 29:  return PCB_LAYER_ID::In28_Cu;
    case 30:  return PCB_LAYER_ID::In29_Cu;
    case 31:  return PCB_LAYER_ID::In30_Cu;
    case 32:  return PCB_LAYER_ID::B_Cu;
    }

    return PCB_LAYER_ID::UNDEFINED_LAYER;
}

bool CADSTAR_PCB_ARCHIVE_LOADER::isLayerSet( const LAYER_ID& aCadstarLayerID )
{
    wxCHECK( Assignments.Layerdefs.Layers.find( aCadstarLayerID )
                     != Assignments.Layerdefs.Layers.end(),
            false );

    LAYER& layer = Assignments.Layerdefs.Layers.at( aCadstarLayerID );

    switch( layer.Type )
    {
    case LAYER_TYPE::ALLDOC:
    case LAYER_TYPE::ALLELEC:
    case LAYER_TYPE::ALLLAYER:
        return true;

    default:
        return false;
    }

    return false;
}


PCB_LAYER_ID CADSTAR_PCB_ARCHIVE_LOADER::getKiCadLayer( const LAYER_ID& aCadstarLayerID )
{
    if( Assignments.Layerdefs.Layers.find( aCadstarLayerID ) != Assignments.Layerdefs.Layers.end() )
    {
        if( Assignments.Layerdefs.Layers.at( aCadstarLayerID ).Type == LAYER_TYPE::NOLAYER )
            //The "no layer" is common for CADSTAR documentation symbols
            //map it to undefined layer for later processing
            return PCB_LAYER_ID::UNDEFINED_LAYER;
    }

    wxCHECK( mLayermap.find( aCadstarLayerID ) != mLayermap.end(), PCB_LAYER_ID::UNDEFINED_LAYER );

    return mLayermap.at(aCadstarLayerID);
}


LSET CADSTAR_PCB_ARCHIVE_LOADER::getKiCadLayerSet( const LAYER_ID& aCadstarLayerID )
{
    LAYER& layer = Assignments.Layerdefs.Layers.at( aCadstarLayerID );

    switch( layer.Type )
    {
    case LAYER_TYPE::ALLDOC:
        return LSET( 4, PCB_LAYER_ID::Dwgs_User, PCB_LAYER_ID::Cmts_User, PCB_LAYER_ID::Eco1_User,
                PCB_LAYER_ID::Eco2_User );

    case LAYER_TYPE::ALLELEC:
        return LSET::AllCuMask();

    case LAYER_TYPE::ALLLAYER:
        return LSET::AllLayersMask();

    default:
        return LSET( getKiCadLayer( aCadstarLayerID ) );
    }
}


void CADSTAR_PCB_ARCHIVE_LOADER::addToGroup(
        const GROUP_ID& aCadstarGroupID, BOARD_ITEM* aKiCadItem )
{
    wxCHECK( mGroupMap.find( aCadstarGroupID ) != mGroupMap.end(), );

    PCB_GROUP* parentGroup = mGroupMap.at( aCadstarGroupID );
    parentGroup->AddItem( aKiCadItem );
}


CADSTAR_PCB_ARCHIVE_LOADER::GROUP_ID CADSTAR_PCB_ARCHIVE_LOADER::createUniqueGroupID(
        const wxString& aName )
{
    wxString groupName = aName;
    int      num       = 0;

    while( mGroupMap.find( groupName ) != mGroupMap.end() )
    {
        groupName = aName + wxT( "_" ) + wxString::Format( "%i", ++num );
    }

    PCB_GROUP* docSymGroup = new PCB_GROUP( mBoard );
    mBoard->Add( docSymGroup );
    docSymGroup->SetName( groupName );
    GROUP_ID groupID( groupName );
    mGroupMap.insert( { groupID, docSymGroup } );

    return groupID;
}
