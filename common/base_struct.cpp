/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2015 Jean-Pierre Charras, jaen-pierre.charras@gipsa-lab.inpg.com
 * Copyright (C) 1992-2019 KiCad Developers, see AUTHORS.txt for contributors.
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
 * @brief Implementation of EDA_ITEM base class for KiCad.
 */

#include <deque>

#include <fctsys.h>
#include <trigo.h>
#include <common.h>
#include <macros.h>
#include <base_screen.h>
#include <bitmaps.h>
#include <trace_helpers.h>
#include <eda_rect.h>

#include <algorithm>


static const unsigned char dummy_png[] = {
 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0xf3, 0xff,
 0x61, 0x00, 0x00, 0x00, 0x5f, 0x49, 0x44, 0x41, 0x54, 0x38, 0xcb, 0x63, 0xf8, 0xff, 0xff, 0x3f,
 0x03, 0x25, 0x98, 0x61, 0x68, 0x1a, 0x00, 0x04, 0x46, 0x40, 0xfc, 0x02, 0x88, 0x45, 0x41, 0x1c,
 0x76, 0x20, 0xfe, 0x01, 0xc4, 0xbe, 0x24, 0x18, 0x60, 0x01, 0xc4, 0x20, 0x86, 0x04, 0x88, 0xc3,
 0x01, 0xe5, 0x04, 0x0c, 0xb8, 0x01, 0x37, 0x81, 0xf8, 0x04, 0x91, 0xf8, 0x0a, 0x54, 0x8f, 0x06,
 0xb2, 0x01, 0x9b, 0x81, 0x78, 0x02, 0x91, 0x78, 0x05, 0x54, 0x8f, 0xca, 0xe0, 0x08, 0x03, 0x36,
 0xa8, 0xbf, 0xec, 0xc8, 0x32, 0x80, 0xcc, 0x84, 0x04, 0x0a, 0xbc, 0x1d, 0x40, 0x2c, 0xc8, 0x30,
 0xf4, 0x33, 0x13, 0x00, 0x6b, 0x1a, 0x46, 0x7b, 0x68, 0xe7, 0x0f, 0x0b, 0x00, 0x00, 0x00, 0x00,
 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

static const BITMAP_OPAQUE dummy_xpm[1] = {{ dummy_png, sizeof( dummy_png ), "dummy_xpm" }};


EDA_ITEM::EDA_ITEM( EDA_ITEM* parent, KICAD_T idType ) :
        m_StructType( idType ),
        m_Status( 0 ),
        m_Parent( parent ),
        m_forceVisible( false ),
        m_Flags( 0 )
{ }


EDA_ITEM::EDA_ITEM( KICAD_T idType ) :
        m_StructType( idType ),
        m_Status( 0 ),
        m_Parent( nullptr ),
        m_forceVisible( false ),
        m_Flags( 0 )
{ }


EDA_ITEM::EDA_ITEM( const EDA_ITEM& base ) :
        m_Uuid( base.m_Uuid ),
        m_StructType( base.m_StructType ),
        m_Status( base.m_Status ),
        m_Parent( base.m_Parent ),
        m_forceVisible( base.m_forceVisible ),
        m_Flags( base.m_Flags )
{ }


void EDA_ITEM::SetModified()
{
    SetFlags( IS_CHANGED );

    // If this a child object, then the parent modification state also needs to be set.
    if( m_Parent )
        m_Parent->SetModified();
}


const EDA_RECT EDA_ITEM::GetBoundingBox() const
{
    // return a zero-sized box per default. derived classes should override
    // this
    return EDA_RECT( wxPoint( 0, 0 ), wxSize( 0, 0 ) );
}


EDA_ITEM* EDA_ITEM::Clone() const
{
    wxCHECK_MSG( false, NULL, wxT( "Clone not implemented in derived class " ) + GetClass() +
                 wxT( ".  Bad programmer!" ) );
}


// see base_struct.h
// many classes inherit this method, be careful:
//TODO (snh): Fix this to use std::set instead of C-style vector
SEARCH_RESULT EDA_ITEM::Visit( INSPECTOR inspector, void* testData, const KICAD_T scanTypes[] )
{
#if 0 && defined(DEBUG)
    std::cout << GetClass().mb_str() << ' ';
#endif

    if( IsType( scanTypes ) )
    {
        if( SEARCH_RESULT::QUIT == inspector( this, testData ) )
            return SEARCH_RESULT::QUIT;
    }

    return SEARCH_RESULT::CONTINUE;
}


wxString EDA_ITEM::GetSelectMenuText( EDA_UNITS aUnits ) const
{
    wxFAIL_MSG( wxT( "GetSelectMenuText() was not overridden for schematic item type " ) +
                GetClass() );

    return wxString( wxT( "Undefined menu text for " ) + GetClass() );
}


bool EDA_ITEM::Matches( const wxString& aText, wxFindReplaceData& aSearchData )
{
    wxString text = aText;
    wxString searchText = aSearchData.GetFindString();

    // Don't match if searching for replaceable item and the item doesn't support text replace.
    if( (aSearchData.GetFlags() & FR_SEARCH_REPLACE) && !IsReplaceable() )
        return false;

    if( aSearchData.GetFlags() & wxFR_WHOLEWORD )
        return aText.IsSameAs( searchText, aSearchData.GetFlags() & wxFR_MATCHCASE );

    if( aSearchData.GetFlags() & FR_MATCH_WILDCARD )
    {
        if( aSearchData.GetFlags() & wxFR_MATCHCASE )
            return text.Matches( searchText );

        return text.MakeUpper().Matches( searchText.MakeUpper() );
    }

    if( aSearchData.GetFlags() & wxFR_MATCHCASE )
        return aText.Find( searchText ) != wxNOT_FOUND;

    return text.MakeUpper().Find( searchText.MakeUpper() ) != wxNOT_FOUND;
}


bool EDA_ITEM::Replace( wxFindReplaceData& aSearchData, wxString& aText )
{
    wxString searchString = (aSearchData.GetFlags() & wxFR_MATCHCASE) ? aText : aText.Upper();

    int result = searchString.Find( (aSearchData.GetFlags() & wxFR_MATCHCASE) ?
                                    aSearchData.GetFindString() :
                                    aSearchData.GetFindString().Upper() );

    if( result == wxNOT_FOUND )
        return false;

    wxString prefix = aText.Left( result );
    wxString suffix;

    if( aSearchData.GetFindString().length() + result < aText.length() )
        suffix = aText.Right( aText.length() - ( aSearchData.GetFindString().length() + result ) );

    wxLogTrace( traceFindReplace, wxT( "Replacing '%s', prefix '%s', replace '%s', suffix '%s'." ),
                GetChars( aText ), GetChars( prefix ), GetChars( aSearchData.GetReplaceString() ),
                GetChars( suffix ) );

    aText = prefix + aSearchData.GetReplaceString() + suffix;

    return true;
}


bool EDA_ITEM::operator<( const EDA_ITEM& aItem ) const
{
    wxFAIL_MSG( wxString::Format( wxT( "Less than operator not defined for item type %s." ),
                                  GetChars( GetClass() ) ) );

    return false;
}

EDA_ITEM& EDA_ITEM::operator=( const EDA_ITEM& aItem )
{
    // do not call initVars()

    m_StructType = aItem.m_StructType;
    m_Flags      = aItem.m_Flags;
    m_Status     = aItem.m_Status;
    m_Parent     = aItem.m_Parent;
    m_forceVisible = aItem.m_forceVisible;

    return *this;
}

const BOX2I EDA_ITEM::ViewBBox() const
{
    // Basic fallback
    return BOX2I( VECTOR2I( GetBoundingBox().GetOrigin() ),
                  VECTOR2I( GetBoundingBox().GetSize() ) );
}


void EDA_ITEM::ViewGetLayers( int aLayers[], int& aCount ) const
{
    // Basic fallback
    aCount      = 1;
    aLayers[0]  = 0;
}

BITMAP_DEF EDA_ITEM::GetMenuImage() const
{
    return dummy_xpm;
}

#if defined(DEBUG)

void EDA_ITEM::ShowDummy( std::ostream& os ) const
{
    // XML output:
    wxString s = GetClass();

    os << '<' << s.Lower().mb_str() << ">"
       << " Need ::Show() override for this class "
       << "</" << s.Lower().mb_str() << ">\n";
}


std::ostream& EDA_ITEM::NestedSpace( int nestLevel, std::ostream& os )
{
    for( int i = 0; i<nestLevel; ++i )
        os << "  ";

    // number of spaces here controls indent per nest level

    return os;
}

#endif


/******************/
/* Class EDA_RECT */
/******************/

void EDA_RECT::Normalize()
{
    if( m_Size.y < 0 )
    {
        m_Size.y = -m_Size.y;
        m_Pos.y -= m_Size.y;
    }

    if( m_Size.x < 0 )
    {
        m_Size.x = -m_Size.x;
        m_Pos.x -= m_Size.x;
    }
}


void EDA_RECT::Move( const wxPoint& aMoveVector )
{
    m_Pos += aMoveVector;
}


bool EDA_RECT::Contains( const wxPoint& aPoint ) const
{
    wxPoint rel_pos = aPoint - m_Pos;
    wxSize size     = m_Size;

    if( size.x < 0 )
    {
        size.x    = -size.x;
        rel_pos.x += size.x;
    }

    if( size.y < 0 )
    {
        size.y    = -size.y;
        rel_pos.y += size.y;
    }

    return (rel_pos.x >= 0) && (rel_pos.y >= 0) && ( rel_pos.y <= size.y) && ( rel_pos.x <= size.x);
}


bool EDA_RECT::Contains( const EDA_RECT& aRect ) const
{
    return Contains( aRect.GetOrigin() ) && Contains( aRect.GetEnd() );
}


bool EDA_RECT::Intersects( const wxPoint& aPoint1, const wxPoint& aPoint2 ) const
{
    wxPoint point2, point4;

    if( Contains( aPoint1 ) || Contains( aPoint2 ) )
        return true;

    point2.x = GetEnd().x;
    point2.y = GetOrigin().y;
    point4.x = GetOrigin().x;
    point4.y = GetEnd().y;

    //Only need to test 3 sides since a straight line cant enter and exit on same side
    if( SegmentIntersectsSegment( aPoint1, aPoint2, GetOrigin() , point2 ) )
        return true;

    if( SegmentIntersectsSegment( aPoint1, aPoint2, point2      , GetEnd() ) )
        return true;

    if( SegmentIntersectsSegment( aPoint1, aPoint2, GetEnd()    , point4 ) )
        return true;

    return false;
}


bool EDA_RECT::Intersects( const wxPoint& aPoint1, const wxPoint& aPoint2,
                           wxPoint* aIntersection1, wxPoint* aIntersection2 ) const
{
    wxPoint point2, point4;

    point2.x = GetEnd().x;
    point2.y = GetOrigin().y;
    point4.x = GetOrigin().x;
    point4.y = GetEnd().y;

    bool intersects = false;

    wxPoint* aPointToFill = aIntersection1;

    if( SegmentIntersectsSegment( aPoint1, aPoint2, GetOrigin(), point2, aPointToFill ) )
        intersects = true;

    if( intersects )
        aPointToFill = aIntersection2;

    if( SegmentIntersectsSegment( aPoint1, aPoint2, point2, GetEnd(), aPointToFill ) )
        intersects = true;

    if( intersects )
        aPointToFill = aIntersection2;

    if( SegmentIntersectsSegment( aPoint1, aPoint2, GetEnd(), point4, aPointToFill ) )
        intersects = true;

    if( intersects )
        aPointToFill = aIntersection2;

    if( SegmentIntersectsSegment( aPoint1, aPoint2, point4, GetOrigin(), aPointToFill ) )
        intersects = true;

    return intersects;
}


bool EDA_RECT::Intersects( const EDA_RECT& aRect ) const
{
    if( !m_init )
        return false;

    // this logic taken from wxWidgets' geometry.cpp file:
    bool rc;
    EDA_RECT me(*this);
    EDA_RECT rect(aRect);
    me.Normalize();         // ensure size is >= 0
    rect.Normalize();       // ensure size is >= 0

    // calculate the left common area coordinate:
    int  left   = std::max( me.m_Pos.x, rect.m_Pos.x );
    // calculate the right common area coordinate:
    int  right  = std::min( me.m_Pos.x + me.m_Size.x, rect.m_Pos.x + rect.m_Size.x );
    // calculate the upper common area coordinate:
    int  top    = std::max( me.m_Pos.y, aRect.m_Pos.y );
    // calculate the lower common area coordinate:
    int  bottom = std::min( me.m_Pos.y + me.m_Size.y, rect.m_Pos.y + rect.m_Size.y );

    // if a common area exists, it must have a positive (null accepted) size
    if( left <= right && top <= bottom )
        rc = true;
    else
        rc = false;

    return rc;
}


bool EDA_RECT::Intersects( const EDA_RECT& aRect, double aRot ) const
{
    if( !m_init )
        return false;

    /* Most rectangles will be axis aligned.
     * It is quicker to check for this case and pass the rect
     * to the simpler intersection test
     */

    // Prevent floating point comparison errors
    static const double ROT_EPS = 0.000000001;

    static const double ROT_PARALLEL[] = { -3600, -1800, 0, 1800, 3600 };
    static const double ROT_PERPENDICULAR[] = { -2700, -900, 0, 900, 2700 };

    NORMALIZE_ANGLE_POS<double>( aRot );

    // Test for non-rotated rectangle
    for( int ii = 0; ii < 5; ii++ )
    {
        if( std::fabs( aRot - ROT_PARALLEL[ii] ) < ROT_EPS )
        {
            return Intersects( aRect );
        }
    }

    // Test for rectangle rotated by multiple of 90 degrees
    for( int jj = 0; jj < 4; jj++ )
    {
        if( std::fabs( aRot - ROT_PERPENDICULAR[jj] ) < ROT_EPS )
        {
            EDA_RECT rotRect;

            // Rotate the supplied rect by 90 degrees
            rotRect.SetOrigin( aRect.Centre() );
            rotRect.Inflate( aRect.GetHeight(), aRect.GetWidth() );
            return Intersects( rotRect );
        }
    }

    /* There is some non-orthogonal rotation.
     * There are three cases to test:
     * A) One point of this rect is inside the rotated rect
     * B) One point of the rotated rect is inside this rect
     * C) One of the sides of the rotated rect intersect this
     */

    wxPoint corners[4];

    /* Test A : Any corners exist in rotated rect? */

    corners[0] = m_Pos;
    corners[1] = m_Pos + wxPoint( m_Size.x, 0 );
    corners[2] = m_Pos + wxPoint( m_Size.x, m_Size.y );
    corners[3] = m_Pos + wxPoint( 0, m_Size.y );

    wxPoint rCentre = aRect.Centre();

    for( int i = 0; i < 4; i++ )
    {
        wxPoint delta = corners[i] - rCentre;
        RotatePoint( &delta, -aRot );
        delta += rCentre;

        if( aRect.Contains( delta ) )
        {
            return true;
        }
    }

    /* Test B : Any corners of rotated rect exist in this one? */
    int w = aRect.GetWidth() / 2;
    int h = aRect.GetHeight() / 2;

    // Construct corners around center of shape
    corners[0] = wxPoint( -w, -h );
    corners[1] = wxPoint(  w, -h );
    corners[2] = wxPoint(  w,  h );
    corners[3] = wxPoint( -w,  h );

    // Rotate and test each corner
    for( int j=0; j<4; j++ )
    {
        RotatePoint( &corners[j], aRot );
        corners[j] += rCentre;

        if( Contains( corners[j] ) )
        {
            return true;
        }
    }

    /* Test C : Any sides of rotated rect intersect this */

    if( Intersects( corners[0], corners[1] ) ||
        Intersects( corners[1], corners[2] ) ||
        Intersects( corners[2], corners[3] ) ||
        Intersects( corners[3], corners[0] ) )
    {
        return true;
    }


    return false;
}


const wxPoint EDA_RECT::ClosestPointTo( const wxPoint& aPoint ) const
{
    EDA_RECT me( *this );

    me.Normalize();         // ensure size is >= 0

    // Determine closest point to the circle centre within this rect
    int nx = std::max( me.GetLeft(), std::min( aPoint.x, me.GetRight() ) );
    int ny = std::max( me.GetTop(), std::min( aPoint.y, me.GetBottom() ) );

    return wxPoint( nx, ny );
}


const wxPoint EDA_RECT::FarthestPointTo( const wxPoint& aPoint ) const
{
    EDA_RECT me( *this );

    me.Normalize();         // ensure size is >= 0

    int fx = std::max( std::abs( aPoint.x - me.GetLeft() ), std::abs( aPoint.x - me.GetRight() ) );
    int fy = std::max( std::abs( aPoint.y - me.GetTop() ), std::abs( aPoint.y - me.GetBottom() ) );

    return wxPoint( fx, fy );
}


bool EDA_RECT::IntersectsCircle( const wxPoint& aCenter, const int aRadius ) const
{
    if( !m_init )
        return false;

    wxPoint closest = ClosestPointTo( aCenter );

    double dx = aCenter.x - closest.x;
    double dy = aCenter.y - closest.y;

    double r = (double) aRadius;

    return ( dx * dx + dy * dy ) <= ( r * r );
}


bool EDA_RECT::IntersectsCircleEdge( const wxPoint& aCenter, const int aRadius, const int aWidth ) const
{
    if( !m_init )
        return false;

    EDA_RECT me( *this );
    me.Normalize();         // ensure size is >= 0

    // Test if the circle intersects at all
    if( !IntersectsCircle( aCenter, aRadius + aWidth / 2 ) )
    {
        return false;
    }

    wxPoint farpt = FarthestPointTo( aCenter );
    // Farthest point must be further than the inside of the line
    double fx = (double) farpt.x;
    double fy = (double) farpt.y;

    double r = (double) aRadius - (double) aWidth / 2;

    return ( fx * fx + fy * fy ) > ( r * r );
}


EDA_RECT& EDA_RECT::Inflate( int aDelta )
{
    Inflate( aDelta, aDelta );
    return *this;
}


EDA_RECT& EDA_RECT::Inflate( wxCoord dx, wxCoord dy )
{
    if( m_Size.x >= 0 )
    {
        if( m_Size.x < -2 * dx )
        {
            // Don't allow deflate to eat more width than we have,
            m_Pos.x += m_Size.x / 2;
            m_Size.x = 0;
        }
        else
        {
            // The inflate is valid.
            m_Pos.x  -= dx;
            m_Size.x += 2 * dx;
        }
    }
    else    // size.x < 0:
    {
        if( m_Size.x > -2 * dx )
        {
            // Don't allow deflate to eat more width than we have,
            m_Pos.x -= m_Size.x / 2;
            m_Size.x = 0;
        }
        else
        {
            // The inflate is valid.
            m_Pos.x  += dx;
            m_Size.x -= 2 * dx; // m_Size.x <0: inflate when dx > 0
        }
    }

    if( m_Size.y >= 0 )
    {
        if( m_Size.y < -2 * dy )
        {
            // Don't allow deflate to eat more height than we have,
            m_Pos.y += m_Size.y / 2;
            m_Size.y = 0;
        }
        else
        {
            // The inflate is valid.
            m_Pos.y  -= dy;
            m_Size.y += 2 * dy;
        }
    }
    else    // size.y < 0:
    {
        if( m_Size.y > 2 * dy )
        {
            // Don't allow deflate to eat more height than we have,
            m_Pos.y -= m_Size.y / 2;
            m_Size.y = 0;
        }
        else
        {
            // The inflate is valid.
            m_Pos.y  += dy;
            m_Size.y -= 2 * dy; // m_Size.y <0: inflate when dy > 0
        }
    }

    return *this;
}


void EDA_RECT::Merge( const EDA_RECT& aRect )
{
    if( !m_init )
    {
        if( aRect.IsValid() )
        {
            m_Pos = aRect.GetPosition();
            m_Size = aRect.GetSize();
            m_init = true;
        }
        return;
    }

    Normalize();        // ensure width and height >= 0
    EDA_RECT rect = aRect;
    rect.Normalize();   // ensure width and height >= 0
    wxPoint  end = GetEnd();
    wxPoint  rect_end = rect.GetEnd();

    // Change origin and size in order to contain the given rect
    m_Pos.x = std::min( m_Pos.x, rect.m_Pos.x );
    m_Pos.y = std::min( m_Pos.y, rect.m_Pos.y );
    end.x   = std::max( end.x, rect_end.x );
    end.y   = std::max( end.y, rect_end.y );
    SetEnd( end );
}


void EDA_RECT::Merge( const wxPoint& aPoint )
{
    if( !m_init )
    {
        m_Pos = aPoint;
        m_Size = wxSize( 0, 0 );
        m_init = true;
        return;
    }

    Normalize();        // ensure width and height >= 0

    wxPoint  end = GetEnd();
    // Change origin and size in order to contain the given rect
    m_Pos.x = std::min( m_Pos.x, aPoint.x );
    m_Pos.y = std::min( m_Pos.y, aPoint.y );
    end.x   = std::max( end.x, aPoint.x );
    end.y   = std::max( end.y, aPoint.y );
    SetEnd( end );
}


double EDA_RECT::GetArea() const
{
    return (double) GetWidth() * (double) GetHeight();
}


EDA_RECT EDA_RECT::Common( const EDA_RECT& aRect ) const
{
    EDA_RECT r;

    if( Intersects( aRect ) )
    {
        wxPoint originA( std::min( GetOrigin().x, GetEnd().x ),
                         std::min( GetOrigin().y, GetEnd().y ) );
        wxPoint originB( std::min( aRect.GetOrigin().x, aRect.GetEnd().x ),
                         std::min( aRect.GetOrigin().y, aRect.GetEnd().y ) );
        wxPoint endA( std::max( GetOrigin().x, GetEnd().x ),
                      std::max( GetOrigin().y, GetEnd().y ) );
        wxPoint endB( std::max( aRect.GetOrigin().x, aRect.GetEnd().x ),
                      std::max( aRect.GetOrigin().y, aRect.GetEnd().y ) );

        r.SetOrigin( wxPoint( std::max( originA.x, originB.x ), std::max( originA.y, originB.y ) ) );
        r.SetEnd   ( wxPoint( std::min( endA.x, endB.x ),       std::min( endA.y, endB.y ) ) );
    }

    return r;
}


const EDA_RECT EDA_RECT::GetBoundingBoxRotated( wxPoint aRotCenter, double aAngle ) const
{
    wxPoint corners[4];

    // Build the corners list
    corners[0]   = GetOrigin();
    corners[2]   = GetEnd();
    corners[1].x = corners[0].x;
    corners[1].y = corners[2].y;
    corners[3].x = corners[2].x;
    corners[3].y = corners[0].y;

    // Rotate all corners, to find the bounding box
    for( int ii = 0; ii < 4; ii ++ )
        RotatePoint( &corners[ii], aRotCenter, aAngle );

    // Find the corners bounding box
    wxPoint start = corners[0];
    wxPoint end = corners[0];

    for( int ii = 1; ii < 4; ii ++ )
    {
        start.x = std::min( start.x, corners[ii].x);
        start.y = std::min( start.y, corners[ii].y);
        end.x = std::max( end.x, corners[ii].x);
        end.y = std::max( end.y, corners[ii].y);
    }

    EDA_RECT bbox;
    bbox.SetOrigin( start );
    bbox.SetEnd( end );

    return bbox;
}


static struct EDA_ITEM_DESC
{
    EDA_ITEM_DESC()
    {
        ENUM_MAP<KICAD_T>::Instance()
            .Undefined( TYPE_NOT_INIT )
            .Map( NOT_USED,             wxT( "<not used>" ) )
            .Map( SCREEN_T,             _( "Screen" ) )

            .Map( PCB_MODULE_T,         _( "Footprint" ) )
            .Map( PCB_PAD_T,            _( "Pad" ) )
            .Map( PCB_LINE_T,           _( "Line" ) )
            .Map( PCB_TEXT_T,           _( "Board Text" ) )
            .Map( PCB_MODULE_TEXT_T,    _( "Footprint Text" ) )
            .Map( PCB_MODULE_EDGE_T,    _( "Footprint Graphics" ) )
            .Map( PCB_TRACE_T,          _( "Track" ) )
            .Map( PCB_VIA_T,            _( "Via" ) )
            .Map( PCB_MARKER_T,         _( "Board Marker" ) )
            .Map( PCB_DIM_ALIGNED_T,    _( "Aligned Dimension" ) )
            .Map( PCB_DIM_ORTHOGONAL_T, _( "Orthogonal Dimension" ) )
            .Map( PCB_DIM_CENTER_T,     _( "Center Dimension" ) )
            .Map( PCB_DIM_LEADER_T,     _( "Leader" ) )
            .Map( PCB_TARGET_T,         _( "Target" ) )
            .Map( PCB_ZONE_AREA_T,      _( "Zone" ) )
            .Map( PCB_ITEM_LIST_T,      _( "Item List" ) )
            .Map( PCB_NETINFO_T,        _( "Net Info" ) )
            .Map( PCB_GROUP_T,          _( "Group" ) )

            .Map( SCH_MARKER_T,         _( "Schematic Marker" ) )
            .Map( SCH_JUNCTION_T,       _( "Junction" ) )
            .Map( SCH_NO_CONNECT_T,     _( "No-Connect Flag" ) )
            .Map( SCH_BUS_WIRE_ENTRY_T, _( "Wire Entry" ) )
            .Map( SCH_BUS_BUS_ENTRY_T,  _( "Bus Entry" ) )
            .Map( SCH_LINE_T,           _( "Graphic Line" ) )
            .Map( SCH_BITMAP_T,         _( "Bitmap" ) )
            .Map( SCH_TEXT_T,           _( "Schematic Text" ) )
            .Map( SCH_LABEL_T,          _( "Net Label" ) )
            .Map( SCH_GLOBAL_LABEL_T,   _( "Global Label" ) )
            .Map( SCH_HIER_LABEL_T,     _( "Hierarchical Label" ) )
            .Map( SCH_FIELD_T,          _( "Schematic Field" ) )
            .Map( SCH_COMPONENT_T,      _( "Component" ) )
            .Map( SCH_SHEET_PIN_T,      _( "Sheet Pin" ) )
            .Map( SCH_SHEET_T,          _( "Sheet" ) )

            .Map( SCH_FIELD_LOCATE_REFERENCE_T, _( "Field Locate Reference" ) )
            .Map( SCH_FIELD_LOCATE_VALUE_T,     _( "Field Locate Value" ) )
            .Map( SCH_FIELD_LOCATE_FOOTPRINT_T, _( "Field Locate Footprint" ) )

            .Map( SCH_SCREEN_T,         _( "SCH Screen" ) )

            .Map( LIB_PART_T,           _( "Symbol" ) )
            .Map( LIB_ALIAS_T,          _( "Alias" ) )
            .Map( LIB_ARC_T,            _( "Arc" ) )
            .Map( LIB_CIRCLE_T,         _( "Circle" ) )
            .Map( LIB_TEXT_T,           _( "Symbol Text" ) )
            .Map( LIB_RECTANGLE_T,      _( "Rectangle" ) )
            .Map( LIB_POLYLINE_T,       _( "Polyline" ) )
            .Map( LIB_BEZIER_T,         _( "Bezier" ) )
            .Map( LIB_PIN_T,            _( "Pin" ) )
            .Map( LIB_FIELD_T,          _( "Symbol Field" ) )

            .Map( GERBER_LAYOUT_T,      _( "Gerber Layout" ) )
            .Map( GERBER_DRAW_ITEM_T,   _( "Draw Item" ) )
            .Map( GERBER_IMAGE_T,       _( "Image" ) );

        PROPERTY_MANAGER& propMgr = PROPERTY_MANAGER::Instance();
        REGISTER_TYPE( EDA_ITEM );
        propMgr.AddProperty( new PROPERTY_ENUM<EDA_ITEM, KICAD_T>( "Type",
                    NO_SETTER( EDA_ITEM, KICAD_T ), &EDA_ITEM::Type ) );
    }
} _EDA_ITEM_DESC;

ENUM_TO_WXANY( KICAD_T );
