#include "EventSerialPanel.h"
#include "events/EventSerial.h"
#include "../xLights/outputs/SerialOutput.h"

//(*InternalHeaders(EventSerialPanel)
#include <wx/intl.h>
#include <wx/string.h>
//*)

//(*IdInit(EventSerialPanel)
const long EventSerialPanel::ID_STATICTEXT1 = wxNewId();
const long EventSerialPanel::ID_CHOICE1 = wxNewId();
const long EventSerialPanel::ID_STATICTEXT2 = wxNewId();
const long EventSerialPanel::ID_CHOICE2 = wxNewId();
const long EventSerialPanel::ID_STATICTEXT3 = wxNewId();
const long EventSerialPanel::ID_CHOICE3 = wxNewId();
const long EventSerialPanel::ID_STATICTEXT5 = wxNewId();
const long EventSerialPanel::ID_SPINCTRL1 = wxNewId();
const long EventSerialPanel::ID_STATICTEXT6 = wxNewId();
const long EventSerialPanel::ID_CHOICE4 = wxNewId();
const long EventSerialPanel::ID_STATICTEXT7 = wxNewId();
const long EventSerialPanel::ID_SPINCTRL2 = wxNewId();
//*)

BEGIN_EVENT_TABLE(EventSerialPanel,wxPanel)
	//(*EventTable(EventSerialPanel)
	//*)
END_EVENT_TABLE()

EventSerialPanel::EventSerialPanel(wxWindow* parent,wxWindowID id,const wxPoint& pos,const wxSize& size)
{
	//(*Initialize(EventSerialPanel)
	wxFlexGridSizer* FlexGridSizer1;

	Create(parent, id, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL, _T("id"));
	FlexGridSizer1 = new wxFlexGridSizer(0, 2, 0, 0);
	FlexGridSizer1->AddGrowableCol(1);
	StaticText1 = new wxStaticText(this, ID_STATICTEXT1, _("Protocol:"), wxDefaultPosition, wxDefaultSize, 0, _T("ID_STATICTEXT1"));
	FlexGridSizer1->Add(StaticText1, 1, wxALL|wxALIGN_LEFT|wxALIGN_CENTER_VERTICAL, 5);
	Choice_Protocol = new wxChoice(this, ID_CHOICE1, wxDefaultPosition, wxDefaultSize, 0, 0, 0, wxDefaultValidator, _T("ID_CHOICE1"));
	Choice_Protocol->SetSelection( Choice_Protocol->Append(_("DMX")) );
	Choice_Protocol->Append(_("OpenDMX"));
	FlexGridSizer1->Add(Choice_Protocol, 1, wxALL|wxEXPAND, 5);
	StaticText2 = new wxStaticText(this, ID_STATICTEXT2, _("Port:"), wxDefaultPosition, wxDefaultSize, 0, _T("ID_STATICTEXT2"));
	FlexGridSizer1->Add(StaticText2, 1, wxALL|wxALIGN_LEFT|wxALIGN_CENTER_VERTICAL, 5);
	Choice_CommPort = new wxChoice(this, ID_CHOICE2, wxDefaultPosition, wxDefaultSize, 0, 0, 0, wxDefaultValidator, _T("ID_CHOICE2"));
	FlexGridSizer1->Add(Choice_CommPort, 1, wxALL|wxEXPAND, 5);
	StaticText3 = new wxStaticText(this, ID_STATICTEXT3, _("Speed:"), wxDefaultPosition, wxDefaultSize, 0, _T("ID_STATICTEXT3"));
	FlexGridSizer1->Add(StaticText3, 1, wxALL|wxALIGN_LEFT|wxALIGN_CENTER_VERTICAL, 5);
	Choice_Speed = new wxChoice(this, ID_CHOICE3, wxDefaultPosition, wxDefaultSize, 0, 0, 0, wxDefaultValidator, _T("ID_CHOICE3"));
	FlexGridSizer1->Add(Choice_Speed, 1, wxALL|wxEXPAND, 5);
	StaticText5 = new wxStaticText(this, ID_STATICTEXT5, _("Channel:"), wxDefaultPosition, wxDefaultSize, 0, _T("ID_STATICTEXT5"));
	FlexGridSizer1->Add(StaticText5, 1, wxALL|wxALIGN_LEFT|wxALIGN_CENTER_VERTICAL, 5);
	SpinCtrl_Channel = new wxSpinCtrl(this, ID_SPINCTRL1, _T("1"), wxDefaultPosition, wxDefaultSize, 0, 1, 512, 1, _T("ID_SPINCTRL1"));
	SpinCtrl_Channel->SetValue(_T("1"));
	FlexGridSizer1->Add(SpinCtrl_Channel, 1, wxALL|wxEXPAND, 5);
	StaticText6 = new wxStaticText(this, ID_STATICTEXT6, _("Test:"), wxDefaultPosition, wxDefaultSize, 0, _T("ID_STATICTEXT6"));
	FlexGridSizer1->Add(StaticText6, 1, wxALL|wxALIGN_LEFT|wxALIGN_CENTER_VERTICAL, 5);
	Choice_Test = new wxChoice(this, ID_CHOICE4, wxDefaultPosition, wxDefaultSize, 0, 0, 0, wxDefaultValidator, _T("ID_CHOICE4"));
	Choice_Test->SetSelection( Choice_Test->Append(_("Equals")) );
	Choice_Test->Append(_("Less Than"));
	Choice_Test->Append(_("Less Than or Equals"));
	Choice_Test->Append(_("Greater Than"));
	Choice_Test->Append(_("Greater Than or Equals"));
	Choice_Test->Append(_("Not Equals"));
	Choice_Test->Append(_("Continuous"));
	Choice_Test->Append(_("On Change"));
	FlexGridSizer1->Add(Choice_Test, 1, wxALL|wxEXPAND, 5);
	StaticText7 = new wxStaticText(this, ID_STATICTEXT7, _("Value:"), wxDefaultPosition, wxDefaultSize, 0, _T("ID_STATICTEXT7"));
	FlexGridSizer1->Add(StaticText7, 1, wxALL|wxALIGN_LEFT|wxALIGN_CENTER_VERTICAL, 5);
	SpinCtrl_Value = new wxSpinCtrl(this, ID_SPINCTRL2, _T("255"), wxDefaultPosition, wxDefaultSize, 0, 0, 255, 255, _T("ID_SPINCTRL2"));
	SpinCtrl_Value->SetValue(_T("255"));
	FlexGridSizer1->Add(SpinCtrl_Value, 1, wxALL|wxEXPAND, 5);
	SetSizer(FlexGridSizer1);
	FlexGridSizer1->Fit(this);
	FlexGridSizer1->SetSizeHints(this);
	//*)

    auto ports = SerialOutput::GetPossibleSerialPorts();
    for (auto it = ports.begin(); it != ports.end(); ++it)
    {
        Choice_CommPort->Append(*it);
    }
    Choice_CommPort->SetSelection(0);

    auto speeds = SerialOutput::GetPossibleBaudRates();
    for (auto it = speeds.begin(); it != speeds.end(); ++it)
    {
        Choice_Speed->Append(*it);
    }
    Choice_Speed->SetStringSelection("19200");
}

EventSerialPanel::~EventSerialPanel()
{
	//(*Destroy(EventSerialPanel)
	//*)
}

bool EventSerialPanel::ValidateWindow()
{
    return true;
}

void EventSerialPanel::Save(EventBase* event)
{
    EventSerial* e = (EventSerial*)event;
    e->SetProtocol(Choice_Protocol->GetStringSelection().ToStdString());
    e->SetCommPort(Choice_CommPort->GetStringSelection().ToStdString());
    e->SetCondition(Choice_Test->GetStringSelection().ToStdString());
    e->SetSpeed(wxAtoi(Choice_Speed->GetStringSelection()));
    e->SetChannel(SpinCtrl_Channel->GetValue());
    e->SetThreshold(SpinCtrl_Value->GetValue());
}

void EventSerialPanel::Load(EventBase* event)
{
    EventSerial* e = (EventSerial*)event;
    Choice_Protocol->SetStringSelection(e->GetProtocol());
    Choice_CommPort->SetStringSelection(e->GetCommPort());
    Choice_Test->SetStringSelection(e->GetCondition());
    Choice_Speed->SetStringSelection(wxString::Format("%d", e->GetSpeed()));
    SpinCtrl_Channel->SetValue(e->GetChannel());
    SpinCtrl_Value->SetValue(e->GetThreshold());
}
