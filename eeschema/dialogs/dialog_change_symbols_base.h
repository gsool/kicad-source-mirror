///////////////////////////////////////////////////////////////////////////
// C++ code generated with wxFormBuilder (version Oct 26 2018)
// http://www.wxformbuilder.org/
//
// PLEASE DO *NOT* EDIT THIS FILE!
///////////////////////////////////////////////////////////////////////////

#pragma once

#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/intl.h>
class WX_HTML_REPORT_PANEL;

#include "dialog_shim.h"
#include <wx/string.h>
#include <wx/radiobut.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/textctrl.h>
#include <wx/bmpbuttn.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/button.h>
#include <wx/gbsizer.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/checkbox.h>
#include <wx/statbox.h>
#include <wx/panel.h>
#include <wx/dialog.h>

///////////////////////////////////////////////////////////////////////////


///////////////////////////////////////////////////////////////////////////////
/// Class DIALOG_CHANGE_SYMBOLS_BASE
///////////////////////////////////////////////////////////////////////////////
class DIALOG_CHANGE_SYMBOLS_BASE : public DIALOG_SHIM
{
	private:

	protected:
		wxBoxSizer* m_mainSizer;
		wxGridBagSizer* m_matchSizer;
		wxRadioButton* m_matchAll;
		wxRadioButton* m_matchBySelection;
		wxRadioButton* m_matchByReference;
		wxTextCtrl* m_specifiedReference;
		wxRadioButton* m_matchByValue;
		wxTextCtrl* m_specifiedValue;
		wxRadioButton* m_matchById;
		wxTextCtrl* m_specifiedId;
		wxBitmapButton* m_matchIdBrowserButton;
		wxStaticLine* m_staticline1;
		wxBoxSizer* m_newIdSizer;
		wxTextCtrl* m_newId;
		wxBitmapButton* m_newIdBrowserButton;
		wxStaticBoxSizer* m_updateOptionsSizer;
		wxCheckBox* m_removeExtraBox;
		wxCheckBox* m_resetEmptyFields;
		wxCheckBox* m_resetFieldVisibilities;
		wxCheckBox* m_resetFieldEffects;
		wxCheckBox* m_resetFieldPositions;
		WX_HTML_REPORT_PANEL* m_messagePanel;
		wxStdDialogButtonSizer* m_sdbSizer;
		wxButton* m_sdbSizerOK;
		wxButton* m_sdbSizerCancel;

		// Virtual event handlers, overide them in your derived class
		virtual void onMatchByReference( wxCommandEvent& event ) { event.Skip(); }
		virtual void onMatchByValue( wxCommandEvent& event ) { event.Skip(); }
		virtual void onMatchById( wxCommandEvent& event ) { event.Skip(); }
		virtual void launchMatchIdSymbolBrowser( wxCommandEvent& event ) { event.Skip(); }
		virtual void launchNewIdSymbolBrowser( wxCommandEvent& event ) { event.Skip(); }
		virtual void onOkButtonClicked( wxCommandEvent& event ) { event.Skip(); }


	public:

		DIALOG_CHANGE_SYMBOLS_BASE( wxWindow* parent, wxWindowID id = wxID_ANY, const wxString& title = _("%s"), const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxSize( -1,-1 ), long style = wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER );
		~DIALOG_CHANGE_SYMBOLS_BASE();

};

