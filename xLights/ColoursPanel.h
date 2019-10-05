#pragma once

//(*Headers(ColoursPanel)
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
//*)

#include <wx/dir.h>

class xLightsFrame;

class ColoursPanel: public wxPanel
{
    std::list<std::string> _colours;

    int AddBaseColours();
    void ProcessColourCurveDir(wxDir& directory, bool subdirs);
    void ProcessPaletteDir(wxDir& directory, bool subdirs);
    std::string GetPaletteFolder(const std::string& showFolder);
    int UpdateButtons();
    void ParsePalette(const std::string& pal);

	public:

		ColoursPanel(wxWindow* parent,wxWindowID id=wxID_ANY,const wxPoint& pos=wxDefaultPosition,const wxSize& size=wxDefaultSize);
		virtual ~ColoursPanel();
		void UpdateColourButtons(bool reload, xLightsFrame* xlights);
        int AddColour(const std::string& colour);

		//(*Declarations(ColoursPanel)
		wxFlexGridSizer* FlexGridSizer1;
		wxFlexGridSizer* FlexGridSizer2;
		wxGridSizer* GridSizer1;
		wxPanel* Panel_Sizer;
		wxScrolledWindow* ScrolledWindow1;
		//*)

	protected:

		//(*Identifiers(ColoursPanel)
		static const long ID_SCROLLEDWINDOW1;
		static const long ID_PANEL1;
		//*)

	private:
		//(*Handlers(ColoursPanel)
		void OnResize(wxSizeEvent& event);
		//*)

		DECLARE_EVENT_TABLE()
};

