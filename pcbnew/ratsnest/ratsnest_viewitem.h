/*
 * This program source code file is part of KICAD, a free EDA CAD application.
 *
 * Copyright (C) 2013 CERN
 * @author Maciej Suminski <maciej.suminski@cern.ch>
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

/**
 * @file ratsnest_viewitem.h
 * @brief Class that draws missing connections on a PCB.
 */

#ifndef RATSNEST_VIEWITEM_H
#define RATSNEST_VIEWITEM_H

#include <memory>
#include <base_struct.h>
#include <math/vector2d.h>

class GAL;
class CONNECTIVITY_DATA;

namespace KIGFX
{
class RATSNEST_VIEWITEM : public EDA_ITEM
{
public:
    RATSNEST_VIEWITEM( std::shared_ptr<CONNECTIVITY_DATA> aData );

    /// @copydoc VIEW_ITEM::ViewBBox()
    const BOX2I ViewBBox() const override;

    /// @copydoc VIEW_ITEM::ViewDraw()
    void ViewDraw( int aLayer, KIGFX::VIEW* aView ) const override;

    /// @copydoc VIEW_ITEM::ViewGetLayers()
    void ViewGetLayers( int aLayers[], int& aCount ) const override;

    bool HitTest( const wxPoint& aPoint, int aAccuracy = 0 ) const override
    {
        return false;   // Not selectable
    }

#if defined(DEBUG)
    /// @copydoc EDA_ITEM::Show()
    void Show( int x, std::ostream& st ) const override { }
#endif

    /** Get class name
     * @return  string "RATSNEST_VIEWITEM"
     */
    virtual wxString GetClass() const override
    {
        return wxT( "RATSNEST_VIEWITEM" );
    }

protected:
    ///> Object containing ratsnest data.
    std::shared_ptr<CONNECTIVITY_DATA> m_data;
};

}   // namespace KIGFX

#endif /* RATSNEST_VIEWITEM_H */
