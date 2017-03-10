#include "ScheduleManager.h"
#include <wx/xml/xml.h>
#include <wx/msgdlg.h>
#include <wx/log.h>
#include "ScheduleOptions.h"
#include "PlayList/PlayList.h"
#include "../xLights/outputs/OutputManager.h"
#include "../xLights/outputs/Output.h"
#include "PlayList/PlayListStep.h"
#include "RunningSchedule.h"
#include <log4cpp/Category.hh>
#include <wx/dir.h>
#include <wx/file.h>
#include "../xLights/xLightsVersion.h"
#include "../xLights/AudioManager.h"
#include "xScheduleMain.h"
#include "xScheduleApp.h"
#include <wx/config.h>
#include <wx/sckaddr.h>
#include <wx/socket.h>
#include "UserButton.h"
#include "FSEQFile.h"
#include "OutputProcess.h"
#include <wx/filename.h>
#include <wx/mimetype.h>
#include "PlayList/PlayListItemAudio.h"
#include "PlayList/PlayListItemFSEQ.h"
#include "PlayList/PlayListItemFSEQVideo.h"
#include <wx/stdpaths.h>
#include "PlayList/PlayListItemVideo.h"
#include "Xyzzy.h"
#include "PlayList/PlayListItemText.h"
#include "Control.h"
#include "../xLights/outputs/IPOutput.h"

ScheduleManager::ScheduleManager(const std::string& showDir)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.info("Loading schedule from %s.", (const char *)_showDir.c_str());

    // prime fix file with our show directory for any filename fixups
    FSEQFile::FixFile(showDir, "");

    _backgroundPlayList = nullptr;
    _queuedSongs = new PlayList();
    _queuedSongs->SetName("Song Queue");
    _fppSyncMaster = nullptr;
    _fppSyncSlave = nullptr;
    _manualOTL = -1;
    _immediatePlay = nullptr;
    _scheduleOptions = nullptr;
    _showDir = showDir;
    _startTime = wxGetUTCTimeMillis().GetLo();
    _outputManager = nullptr;
    _buffer = nullptr;
    _brightness = 100;
    _lastBrightness = 100;
    _xyzzy = nullptr;
    _lastXyzzyCommand = wxDateTime::Now();

    wxConfigBase* config = wxConfigBase::Get();
    _mode = (SYNCMODE)config->ReadLong("SyncMode", SYNCMODE::STANDALONE);

    if (_mode == SYNCMODE::FPPMASTER) OpenFPPSyncSendSocket();
    if (_mode == SYNCMODE::FPPSLAVE) OpenFPPSyncListenSocket();

    wxLogNull logNo; //kludge: avoid "error 0" message from wxWidgets after new file is written
    _lastSavedChangeCount = 0;
    _changeCount = 0;
	wxXmlDocument doc;
	doc.Load(showDir + "/" + GetScheduleFile());

    std::string backgroundPlayList = "";

    if (doc.IsOk())
    {
        for (wxXmlNode* n = doc.GetRoot()->GetChildren(); n != nullptr; n = n->GetNext())
        {
            if (n->GetName() == "PlayList")
            {
                _playLists.push_back(new PlayList(n));
            }
            else if (n->GetName() == "Options")
            {
                _scheduleOptions = new ScheduleOptions(n);
            }
            else if (n->GetName() == "OutputProcesses")
            {
                for (wxXmlNode* n1 = n->GetChildren(); n1 != nullptr; n1 = n1->GetNext())
                {
                    OutputProcess* op = OutputProcess::CreateFromXml(n1);
                    if (op != nullptr)
                    {
                        _outputProcessing.push_back(op);
                    }
                }
            }
            else if (n->GetName() == "Background")
            {
                backgroundPlayList = n->GetAttribute("PlayList", "");
            }
        }
    }
    else
    {
        logger_base.error("Problem loading xml file %s.", (const char *)(showDir + "/" + GetScheduleFile()).c_str());
    }

    if (backgroundPlayList != "")
    {
        _backgroundPlayList = new PlayList(*GetPlayList(backgroundPlayList));
    }

    if (_scheduleOptions == nullptr)
    {
        _scheduleOptions = new ScheduleOptions();
    }

    _outputManager = new OutputManager();
    _outputManager->Load(_showDir, _scheduleOptions->IsSync());
    logger_base.info("Loaded outputs from %s.", (const char *)(_showDir + "/" + _outputManager->GetNetworksFileName()).c_str());

    wxString localIP;
    config->Read("xLightsLocalIP", &localIP, "");
    if (localIP != "")
    {
        _outputManager->SetForceFromIP(localIP.ToStdString());
        logger_base.info("Forcing output via %s.", (const char *)localIP.c_str());
    }

    if (_scheduleOptions->IsSendOffWhenNotRunning())
    {
        _outputManager->StartOutput();
        ManageBackground();
        logger_base.info("Started outputting to lights.");
        StartVirtualMatrices();
    }

    // This is out frame data buffer ... it cannot be resized
    logger_base.info("Allocated frame buffer of %ld bytes", _outputManager->GetTotalChannels());
    _buffer = (wxByte*)malloc(_outputManager->GetTotalChannels());
    memset(_buffer, 0x00, _outputManager->GetTotalChannels());
}

void ScheduleManager::AddPlayList(PlayList* playlist)
{
    _playLists.push_back(playlist);
    _changeCount++;
}

std::list<PlayList*> ScheduleManager::GetPlayLists()
{
    return _playLists;
}

ScheduleManager::~ScheduleManager()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    CloseFPPSyncSendSocket();
    _outputManager->StopOutput();
    StopVirtualMatrices();
    ManageBackground();
    logger_base.info("Stopped outputting to lights.");
    if (IsDirty())
	{
		if (wxMessageBox("Unsaved changes to the schedule. Save now?", "Unsaved changes", wxYES_NO) == wxID_YES)
		{
			Save();
		}
	}

    while (_overlayData.size() > 0)
    {
        auto todelete = _overlayData.front();
        _overlayData.remove(todelete);
        delete todelete;
    }

    if (_xyzzy != nullptr)
    {
        std::string(res);
        _xyzzy->Close(res, "");
        delete _xyzzy;
        _xyzzy = nullptr;
    }

    if (_backgroundPlayList != nullptr)
    {
        _backgroundPlayList->Stop();
        delete _backgroundPlayList;
        _backgroundPlayList = nullptr;
    }

    while (_outputProcessing.size() > 0)
    {
        auto toremove = _outputProcessing.front();
        _outputProcessing.remove(toremove);
        delete toremove;
    }

    while (_playLists.size() > 0)
    {
        auto toremove = _playLists.front();
        _playLists.remove(toremove);
        delete toremove;
    }

    if (_immediatePlay != nullptr)
    {
        delete _immediatePlay;
        _immediatePlay = nullptr;
    }

    if (_queuedSongs != nullptr)
    {
        delete _queuedSongs;
        _queuedSongs = nullptr;
    }

    while (_activeSchedules.size() > 0)
    {
        auto toremove = _activeSchedules.front();
        _activeSchedules.remove(toremove);
        delete toremove;
    }

    delete _scheduleOptions;
    delete _outputManager;
    free(_buffer);

    logger_base.info("Closed schedule.");
}

bool ScheduleManager::IsDirty()
{
    bool res = _lastSavedChangeCount != _changeCount;

    auto it = _playLists.begin();
    while (!res && it != _playLists.end())
    {
        res = res || (*it)->IsDirty();
        ++it;
    }

    res = res || _scheduleOptions->IsDirty();

    for (auto it2 = _outputProcessing.begin(); it2 != _outputProcessing.end(); ++it2)
    {
        res = res || (*it2)->IsDirty();
    }

    return res;
}

void ScheduleManager::Save()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    wxXmlDocument doc;
    wxXmlNode* root = new wxXmlNode(nullptr, wxXML_ELEMENT_NODE, "xSchedule");
    doc.SetRoot(root);

    root->AddChild(_scheduleOptions->Save());

	for(auto it = _playLists.begin(); it != _playLists.end(); ++it)
	{
		root->AddChild((*it)->Save());
	}

    if (_outputProcessing.size() != 0)
    {
        wxXmlNode* op = new wxXmlNode(nullptr, wxXML_ELEMENT_NODE, "OutputProcesses");

        for (auto it = _outputProcessing.begin(); it != _outputProcessing.end(); ++it)
        {
            op->AddChild((*it)->Save());
        }

        root->AddChild(op);
    }

    if (_backgroundPlayList != nullptr)
    {
        wxXmlNode* background = new wxXmlNode(nullptr, wxXML_ELEMENT_NODE, "Background");
        background->AddAttribute("PlayList", _backgroundPlayList->GetNameNoTime());
        root->AddChild(background);
    }

    doc.Save(_showDir + "/" + GetScheduleFile());
    ClearDirty();
    logger_base.info("Saved Schedule to %s.", (const char*)(_showDir + "/" + GetScheduleFile()).c_str());
}

void ScheduleManager::ClearDirty()
{
    _lastSavedChangeCount = _changeCount;

    for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
    {
        (*it)->ClearDirty();
    }

    _scheduleOptions->ClearDirty();

    for (auto it = _outputProcessing.begin(); it != _outputProcessing.end(); ++it)
    {
        (*it)->ClearDirty();
    }
}

void ScheduleManager::RemovePlayList(PlayList* playlist)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.info("Deleting playlist %s.", (const char*)playlist->GetNameNoTime().c_str());
    _playLists.remove(playlist);
    _changeCount++;
}

PlayList* ScheduleManager::GetRunningPlayList() const
{
    // find the highest priority running playlist
    PlayList* running = nullptr;

    if (_immediatePlay != nullptr && _immediatePlay->IsRunning())
    {
        running = _immediatePlay;
    }
    else if (_queuedSongs->GetSteps().size() > 0 && _queuedSongs->IsRunning())
    {
        running = _queuedSongs;
    }
    else
    {
        if (GetRunningSchedule() != nullptr)
        {
            running = GetRunningSchedule()->GetPlayList();
        }
    }

    return running;
}

void ScheduleManager::StopAll()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.info("Stopping all playlists.");

    SendFPPSync("", 0xFFFFFFFF, 50);

    if (_immediatePlay != nullptr)
    {
        _immediatePlay->Stop();
        delete _immediatePlay;
        _immediatePlay = nullptr;
    }

    if (_queuedSongs->IsRunning())
    {
        _queuedSongs->Stop();
        _queuedSongs->RemoveAllSteps();
    }

    for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
    {
        (*it)->GetPlayList()->Stop();
    }
}

void ScheduleManager::Frame(bool outputframe)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    // timeout xyzzy if no api calls for 15 seconds
    if (_xyzzy != nullptr && (wxDateTime::Now() - _lastXyzzyCommand).GetSeconds() > 15)
    {
        logger_base.info("Stopping xyzzy due to timeout.");

        std::string msg;
        DoXyzzy("close", "", msg, "");
    }

    PlayList* running = GetRunningPlayList();

    if (running != nullptr || _xyzzy != nullptr)
    {
        long msec = wxGetUTCTimeMillis().GetLo() - _startTime;

        if (outputframe)
        {
            memset(_buffer, 0x00, _outputManager->GetTotalChannels()); // clear out any prior frame data
            _outputManager->StartFrame(msec);
        }

        bool done = false;
        if (running != nullptr)
        {
            done = running->Frame(_buffer, _outputManager->GetTotalChannels(), outputframe);

            if (outputframe && _mode == SYNCMODE::FPPMASTER)
            {
                SendFPPSync(running->GetActiveSyncItemFSEQ(), running->GetRunningStep()->GetPosition(), running->GetFrameMS());
                if (running->GetActiveSyncItemMedia() != "")
                {
                    SendFPPSync(running->GetActiveSyncItemMedia(), running->GetRunningStep()->GetPosition(), running->GetFrameMS());
                }
            }

            // for queued songs we must remove the queued song when it finishes
            if (running == _queuedSongs)
            {
                if (_queuedSongs->GetRunningStep() != _queuedSongs->GetSteps().front())
                {
                    _queuedSongs->RemoveStep(_queuedSongs->GetSteps().front());
                    wxCommandEvent event(EVT_DOCHECKSCHEDULE);
                    wxPostEvent(wxGetApp().GetTopWindow(), event);
                    wxCommandEvent event2(EVT_SCHEDULECHANGED);
                    wxPostEvent(wxGetApp().GetTopWindow(), event2);
                }
            }
        }

        if (_backgroundPlayList != nullptr)
        {
            if (!_backgroundPlayList->IsRunning())
            {
                _backgroundPlayList->Start(true);
            }
            _backgroundPlayList->Frame(_buffer, _outputManager->GetTotalChannels(), outputframe);
        }

        if (_xyzzy != nullptr)
        {
            _xyzzy->Frame(_buffer, _outputManager->GetTotalChannels(), outputframe);
        }

        if (outputframe)
        {
            // apply any overlay data
            for (auto it = _overlayData.begin(); it != _overlayData.end(); ++it)
            {
                (*it)->Set(_buffer, _outputManager->GetTotalChannels());
            }

            // apply any output processing
            for (auto it = _outputProcessing.begin(); it != _outputProcessing.end(); ++it)
            {
                (*it)->Frame(_buffer, _outputManager->GetTotalChannels());
            }

            auto vm = GetOptions()->GetVirtualMatrices();
            for (auto it = vm->begin(); it != vm->end(); ++it)
            {
                (*it)->Frame(_buffer, _outputManager->GetTotalChannels());
            }

            if (outputframe && _brightness < 100)
            {
                if (_brightness != _lastBrightness)
                {
                    _lastBrightness = _brightness;
                    CreateBrightnessArray();
                }

                wxByte* pb = _buffer;
                for (int i = 0; i < _outputManager->GetTotalChannels(); ++i)
                {
                    *pb = _brightnessArray[*pb];
                    pb++;
                }
            }

            _outputManager->SetManyChannels(0, _buffer, _outputManager->GetTotalChannels());
            _outputManager->EndFrame();
        }

        if (done)
        {
            SendFPPSync(running->GetActiveSyncItemFSEQ(), 0xFFFFFFFF, running->GetFrameMS());
            SendFPPSync(running->GetActiveSyncItemMedia(), 0xFFFFFFFF, running->GetFrameMS());

            // playlist is done
            StopPlayList(running, false);

            wxCommandEvent event(EVT_SCHEDULECHANGED);
            wxPostEvent(wxGetApp().GetTopWindow(), event);
        }
    }
    else
    {
        if (_scheduleOptions->IsSendOffWhenNotRunning())
        {
            _outputManager->StartFrame(0);
            _outputManager->AllOff();

            if (_backgroundPlayList != nullptr)
            {
                if (!_backgroundPlayList->IsRunning())
                {
                    _backgroundPlayList->Start(true);
                }
                _backgroundPlayList->Frame(_buffer, _outputManager->GetTotalChannels(), outputframe);
            }

            // apply any output processing
            for (auto it = _outputProcessing.begin(); it != _outputProcessing.end(); ++it)
            {
                (*it)->Frame(_buffer, _outputManager->GetTotalChannels());
            }

            if (outputframe && _brightness < 100)
            {
                if (_brightness != _lastBrightness)
                {
                    _lastBrightness = _brightness;
                    CreateBrightnessArray();
                }

                wxByte* pb = _buffer;
                for (int i = 0; i < _outputManager->GetTotalChannels(); ++i)
                {
                    *pb = _brightnessArray[*pb];
                    pb++;
                }
            }

            _outputManager->EndFrame();
        }
    }
}

void ScheduleManager::CreateBrightnessArray()
{
    for (int i = 0; i < 256; i++)
    {
        _brightnessArray[i] = ((i * _brightness) / 100) & 0xFF;
    }
}

bool ScheduleManager::PlayPlayList(PlayList* playlist, size_t& rate, bool loop, const std::string& step, bool forcelast, int plloops, bool random, int steploops)
{
    bool result = true;

    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.info("Playing playlist %s.", (const char*)playlist->GetNameNoTime().c_str());

    if (_immediatePlay != nullptr)
    {
        _immediatePlay->Stop();
        delete _immediatePlay;
        _immediatePlay = nullptr;
    }

    if (_queuedSongs->IsRunning())
    {
        logger_base.info("Suspending queued playlist so immediate can play.");
        _queuedSongs->Suspend(true);
    }

    // this needs to create a copy of everything ... including steps etc
    for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
    {
        if (!(*it)->GetPlayList()->IsSuspended())
        {
            logger_base.info("Suspending playlist %s due to schedule %s so immediate can play.", (const char*)(*it)->GetPlayList()->GetNameNoTime().c_str(), (const char *)(*it)->GetSchedule()->GetName().c_str());
            (*it)->GetPlayList()->Suspend(true);
        }
    }

    _immediatePlay = new PlayList(*playlist);
    _immediatePlay->Start(loop, random, plloops);
    if (step != "")
    {
        _immediatePlay->JumpToStep(step);
        _immediatePlay->GetRunningStep()->SetLoops(steploops);
    }

    if (forcelast)
    {
        _immediatePlay->StopAtEndOfCurrentStep();
    }

    rate = 25; // always start fast
    return result;
}

bool compare_runningschedules(const RunningSchedule* first, const RunningSchedule* second)
{
    return first->GetSchedule()->GetPriority() > second->GetSchedule()->GetPriority();
}

int ScheduleManager::CheckSchedule()
{
    if (_mode == SYNCMODE::FPPSLAVE) return 50;

    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("Checking the schedule ...");

    // check all the schedules and add into the list any that should be in the active schedules list
    for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
    {
        auto schedules = (*it)->GetSchedules();
        for (auto it2 = schedules.begin(); it2 != schedules.end(); ++it2)
        {
            logger_base.debug("   Checking playlist %s schedule %s.", (const char *)(*it)->GetNameNoTime().c_str(), (const char *)(*it2)->GetName().c_str());
            if ((*it2)->CheckActive())
            {
                logger_base.debug("   It should be active.");
                bool found = false;

                for (auto it3 = _activeSchedules.begin(); it3 != _activeSchedules.end(); ++it3)
                {
                    if ((*it3)->GetSchedule()->GetId() == (*it2)->GetId())
                    {
                        found = true;
                        break;
                    }
                }

                if (!found)
                {
                    // is hasnt been active before now
                    RunningSchedule* rs = new RunningSchedule(*it, *it2);
                    _activeSchedules.push_back(rs);
                    rs->GetPlayList()->StartSuspended(rs->GetSchedule()->GetLoop(), rs->GetSchedule()->GetRandom(), rs->GetSchedule()->GetLoops());

                    logger_base.info("   Scheduler starting suspended playlist %s due to schedule %s.", (const char*)(*it)->GetNameNoTime().c_str(),  (const char *)(*it2)->GetName().c_str());
                }
                else
                {
                    logger_base.debug("   It was already in the list.");
                }
            }
        }
    }

    std::list<RunningSchedule*> todelete;
    for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
    {
        if (!(*it)->GetSchedule()->CheckActive())
        {

            if (!(*it)->GetPlayList()->IsRunning())
            {
                logger_base.info("   Scheduler removing playlist %s due to schedule %s.", (const char*)(*it)->GetPlayList()->GetNameNoTime().c_str(), (const char *)(*it)->GetSchedule()->GetName().c_str());
                // this shouldnt be in the list any longer
                todelete.push_back(*it);
            }
            else
            {
                if (!(*it)->GetPlayList()->IsFinishingUp())
                {
                    logger_base.info("   Scheduler telling playlist %s due to schedule %s it is time to finish up.", (const char*)(*it)->GetPlayList()->GetNameNoTime().c_str(), (const char *)(*it)->GetSchedule()->GetName().c_str());
                    (*it)->GetPlayList()->JumpToEndStepsAtEndOfCurrentStep();
                }
            }
        }
    }

    for (auto it = todelete.begin(); it != todelete.end(); ++it)
    {
        _activeSchedules.remove(*it);
        delete *it;
    }

    int framems = 50;

    _activeSchedules.sort(compare_runningschedules);

    if (_immediatePlay == nullptr)
    {
        if (_queuedSongs->GetSteps().size() == 0)
        {
            bool first = true;
            for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
            {
                if (first)
                {
                    if ((*it)->GetPlayList()->IsRunning())
                    {
                        first = false;
                        if ((*it)->GetPlayList()->IsSuspended())
                        {
                            logger_base.info("   Unsuspending playlist %s due to schedule %s.", (const char*)(*it)->GetPlayList()->GetNameNoTime().c_str(), (const char *)(*it)->GetSchedule()->GetName().c_str());
                            framems = (*it)->GetPlayList()->Suspend(false);
                        }
                    }
                }
                else
                {
                    if (!(*it)->GetPlayList()->IsSuspended())
                    {
                        logger_base.info("   Suspending playlist %s due to schedule %s.", (const char*)(*it)->GetPlayList()->GetNameNoTime().c_str(), (const char *)(*it)->GetSchedule()->GetName().c_str());
                        (*it)->GetPlayList()->Suspend(true);
                    }
                }
            }
        }
        else
        {
            // we should be playing our queued song
            // make sure they are all suspended
            for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
            {
                if (!(*it)->GetPlayList()->IsSuspended())
                {
                    logger_base.info("   Suspending playlist %s due to schedule %s so immediate can play.", (const char*)(*it)->GetPlayList()->GetNameNoTime().c_str(), (const char *)(*it)->GetSchedule()->GetName().c_str());
                    (*it)->GetPlayList()->Suspend(true);
                }
            }

            if (!_queuedSongs->IsRunning())
            {
                // we need to start it
                _queuedSongs->Start();
            }
            else if  (_queuedSongs->IsSuspended())
            {
                // we need to unsuspend it
                _queuedSongs->Suspend(false);
            }
            else
            {
                // it is already running
            }

            framems = _queuedSongs->GetRunningStep()->GetFrameMS();
        }
    }
    else
    {
        // make sure they are all suspended
        for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
        {
            if (!(*it)->GetPlayList()->IsSuspended())
            {
                logger_base.info("   Suspending playlist %s due to schedule %s so immediate can play.", (const char*)(*it)->GetPlayList()->GetNameNoTime().c_str(), (const char *)(*it)->GetSchedule()->GetName().c_str());
                (*it)->GetPlayList()->Suspend(true);
            }
        }

        if (_queuedSongs->GetSteps().size() > 0 && !_queuedSongs->IsSuspended())
        {
            logger_base.info("   Suspending queued playlist so immediate can play.");
            _queuedSongs->Suspend(true);
        }

        framems = _immediatePlay->GetRunningStep()->GetFrameMS();
    }

    logger_base.debug("   Active scheduled playlists: %d", _activeSchedules.size());

    return framems;
}

std::string ScheduleManager::FormatTime(size_t timems)
{
    return wxString::Format(wxT("%i:%02i.%03i"), (long)(timems / 60000), (long)((timems % 60000) / 1000), (long)(timems % 1000)).ToStdString();
}

std::string ScheduleManager::GetStatus() const
{
    PlayList* curr = GetRunningPlayList();

    if (!IsSomethingPlaying())
    {
        return "Idle";
    }

    return "Playing " + curr->GetRunningStep()->GetNameNoTime() + " " + curr->GetRunningStep()->GetStatus();
}

PlayList* ScheduleManager::GetPlayList(const std::string& playlist) const
{
    for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
    {
        if (wxString((*it)->GetNameNoTime()).Lower() == wxString(playlist).Lower())
        {
            return *it;
        }
    }

    return nullptr;
}

bool ScheduleManager::IsQueuedPlaylistRunning() const
{
    return GetRunningPlayList() != nullptr && _queuedSongs->GetId() == GetRunningPlayList()->GetId();
}

// localhost/xScheduleCommand?Command=<command>&Parameters=<comma separated parameters>
bool ScheduleManager::Action(const std::string command, const std::string parameters, const std::string& data, PlayList* selplaylist, Schedule* selschedule, size_t& rate, std::string& msg)
{
    bool result = true;

    Command* cmd = _commandManager.GetCommand(command);

    if (cmd == nullptr)
    {
        result = false;
        msg = "Unknown command.";
    }
    else
    {
        if (!cmd->IsValid(parameters, selplaylist, selschedule, this, msg, IsQueuedPlaylistRunning()))
        {
            result = false;
        }
        else
        {
            if (command == "Stop all now")
            {
                StopAll();
            }
            else if (command == "Stop")
            {
                PlayList* p = GetRunningPlayList();
                if (p != nullptr)
                {
                    SendFPPSync("", 0xFFFFFFFF, 50);
                    p->Stop();

                    if (_immediatePlay != nullptr && p->GetId() == _immediatePlay->GetId())
                    {
                        delete _immediatePlay;
                        _immediatePlay = nullptr;
                    }
                    else if (p->GetId() == _queuedSongs->GetId())
                    {
                        p->RemoveAllSteps();
                    }
                }
            }
            else if (command == "Play selected playlist")
            {
                if (selplaylist != nullptr)
                {
                    if (!PlayPlayList(selplaylist, rate))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else if (command == "Play selected playlist looped")
            {
                if (selplaylist != nullptr)
                {
                    if (!PlayPlayList(selplaylist, rate, true))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else if (command == "Play specified playlist")
            {
                PlayList* p = GetPlayList(parameters);

                if (p != nullptr)
                {
                    if (!PlayPlayList(p, rate))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else if (command == "Play specified playlist looped")
            {
                PlayList* p = GetPlayList(parameters);

                if (p != nullptr)
                {
                    if (!PlayPlayList(p, rate, true))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else if (command == "Stop specified playlist")
            {
                PlayList* p = GetPlayList(parameters);

                if (p != nullptr)
                {
                    StopPlayList(p, false);
                }
            }
            else if (command == "Stop specified playlist at end of current step")
            {
                PlayList* p = GetPlayList(parameters);

                if (p != nullptr)
                {
                    StopPlayList(p, true);
                }
            }
            else if (command == "Stop playlist at end of current step")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    StopPlayList(p, true);
                }
            }
            else if (command == "Stop specified playlist at end of current loop")
            {
                PlayList* p = GetPlayList(parameters);

                if (p != nullptr)
                {
                    p->StopAtEndOfThisLoop();
                }
            }
            else if (command == "Jump to play once at end at end of current step and then stop")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    p->JumpToEndStepsAtEndOfCurrentStep();
                }
            }
            else if (command == "Stop playlist at end of current loop")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    p->StopAtEndOfThisLoop();
                }
            }
            else if (command == "Pause")
            {
                result = ToggleCurrentPlayListPause(msg);
            }
            else if (command == "Next step in current playlist")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    if (_queuedSongs != nullptr && p->GetId() == _queuedSongs->GetId())
                    {
                        _queuedSongs->Stop();
                        _queuedSongs->RemoveStep(_queuedSongs->GetSteps().front());
                        _queuedSongs->Start();
                    }
                    else
                    {
                        rate = p->JumpToNextStep();
                    }
                }
            }
            else if (command == "Restart step in current playlist")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    p->RestartCurrentStep();
                }
            }
            else if (command == "Prior step in current playlist")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    rate = p->JumpToPriorStep();
                }
            }
            else if (command == "Toggle loop current step")
            {
                result = ToggleCurrentPlayListStepLoop(msg);
            }
            else if (command == "Play specified step in specified playlist looped")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string pl = split[0].ToStdString();
                std::string step = split[1].ToStdString();

                PlayList* p = GetPlayList(pl);

                if (p != nullptr)
                {
                    if (!PlayPlayList(p, rate, false, step))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                    else
                    {
                        p->LoopStep(step);
                    }
                }
            }
            else if (command == "Run process")
            {
                bool run = false;
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string pl = split[0].ToStdString();
                std::string step = split[1].ToStdString();
                std::string item = split[2].ToStdString();

                if (pl == "" && step == "")
                {
                    PlayListItem* pli = FindRunProcessNamed(item);
                    if (pli != nullptr)
                    {
                        if (pli->GetTitle() == "Run Process")
                        {
                            pli->Start();
                            pli->Frame(nullptr, 0, 50, 50, true);
                            pli->Stop();
                            run = true;
                        }
                    }
                }
                else
                {
                    PlayList* p = GetPlayList(pl);
                    if (p != nullptr)
                    {
                        PlayListStep* pls = p->GetStep(step);

                        if (pls != nullptr)
                        {
                            PlayListItem* pli = pls->GetItem(item);

                            if (pli != nullptr)
                            {
                                if (pli->GetTitle() == "Run Process")
                                {
                                    pli->Start();
                                    pli->Frame(nullptr, 0, 50, 50, true);
                                    pli->Stop();
                                    run = true;
                                }
                            }
                        }
                    }
                }
                if (!run)
                {
                    result = false;
                    msg = "Unable to find run process.";
                }
            }
            else if (command == "Play specified playlist step once only")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string pl = split[0].ToStdString();
                std::string step = split[1].ToStdString();

                PlayList* p = GetPlayList(pl);

                if (p != nullptr)
                {
                    if (!PlayPlayList(p, rate, false, step, true))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else if (command == "Enqueue playlist step")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string pl = split[0].ToStdString();
                std::string step = split[1].ToStdString();

                PlayList* p = GetPlayList(pl);

                if (p != nullptr)
                {
                    PlayListStep* pls = p->GetStep(step);

                    if (pls != nullptr)
                    {
                        if (_queuedSongs->GetSteps().size() == 0 || (_queuedSongs->GetSteps().size() > 0 && _queuedSongs->GetSteps().back()->GetId() != pls->GetId()))
                        {
                            _queuedSongs->AddStep(new PlayListStep(*pls), -1);
                            if (!_queuedSongs->IsRunning())
                            {
                                _queuedSongs->StartSuspended();
                            }
                            wxCommandEvent event(EVT_DOCHECKSCHEDULE);
                            wxPostEvent(wxGetApp().GetTopWindow(), event);
                            wxCommandEvent event2(EVT_SCHEDULECHANGED);
                            wxPostEvent(wxGetApp().GetTopWindow(), event2);
                        }
                        else
                        {
                            result = false;
                            msg = "step is already at the end of the list ... I wont add a duplicate.";
                        }
                    }
                    else
                    {
                        result = false;
                        msg = "Unknown step.";
                    }
                }
            }
            else if (command == "Clear playlist queue")
            {
                if (_queuedSongs->IsRunning())
                {
                    _queuedSongs->Stop();
                }

                _queuedSongs->RemoveAllSteps();
                SendFPPSync("", 0xFFFFFFFF, 50);

                wxCommandEvent event(EVT_DOCHECKSCHEDULE);
                wxPostEvent(wxGetApp().GetTopWindow(), event);
                wxCommandEvent event2(EVT_SCHEDULECHANGED);
                wxPostEvent(wxGetApp().GetTopWindow(), event2);
            }
            else if (command == "Play playlist starting at step")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string pl = split[0].ToStdString();
                std::string step = split[1].ToStdString();

                PlayList* p = GetPlayList(pl);

                if (p != nullptr)
                {
                    if (!PlayPlayList(p, rate, false, step))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else if (command == "Play playlist starting at step looped")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string pl = split[0].ToStdString();
                std::string step = split[1].ToStdString();

                PlayList* p = GetPlayList(pl);

                if (p != nullptr)
                {
                    if (!PlayPlayList(p, rate, true, step))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else if (command == "Play playlist step")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string pl = split[0].ToStdString();
                std::string step = split[1].ToStdString();

                PlayList* p = GetPlayList(pl);

                if (p != nullptr)
                {
                    if (!PlayPlayList(p, rate, false, step, true))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else if (command == "Jump to specified step in current playlist")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    rate = p->JumpToStep(parameters);
                }
            }
            else if (command == "Jump to specified step in current playlist at the end of current step")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    p->JumpToStepAtEndOfCurrentStep(parameters);
                }
            }
            else if (command == "Jump to random step in current playlist")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    auto r = p->GetRandomStep();
                    if (r != nullptr)
                    {
                        rate = p->JumpToStep(r->GetNameNoTime());
                    }
                }
            }
            else if (command == "Jump to random step in specified playlist")
            {
                PlayList* p = GetPlayList(parameters);

                if (p != nullptr)
                {
                    auto r = p->GetRandomStep();
                    if (r != nullptr)
                    {
                        rate = p->JumpToStep(r->GetNameNoTime());
                    }
                }
            }
            else if (command == "Add to the current schedule n minutes")
            {
                RunningSchedule *rs = GetRunningSchedule();
                if (rs != nullptr && rs->GetSchedule() != nullptr)
                {
                    rs->GetSchedule()->AddMinsToEndTime(wxAtoi(parameters));
                }
            }
            else if (command == "Set volume to")
            {
                int volume = wxAtoi(parameters);
                SetVolume(volume);
            }
            else if (command == "Adjust volume by")
            {
                int volume = wxAtoi(parameters);
                AdjustVolumeBy(volume);
            }
            else if (command == "Toggle output to lights")
            {
                result = ToggleOutputToLights(msg);
            }
            else if (command == "Toggle mute")
            {
                ToggleMute();
            }
            else if (command == "Increase brightness by n%")
            {
                int by = wxAtoi(parameters);
                AdjustBrightness(by);
            }
            else if (command ==  "Set brightness to n%")
            {
                int b = wxAtoi(parameters);
                SetBrightness(b);
            }
            else if (command == "Toggle current playlist random")
            {
                result = ToggleCurrentPlayListRandom(msg);
            }
            else if (command == "Toggle current playlist loop")
            {
                result = ToggleCurrentPlayListLoop(msg);
            }
            else if (command == "Save schedule")
            {
                Save();
            }
            else if (command == "Run command at end of current step")
            {
                PlayList* p = GetRunningPlayList();

                if (p != nullptr)
                {
                    wxArrayString parms = wxSplit(parameters, ',');

                    if (parms.Count() > 0)
                    {
                        std::string newparms = "";
                        for (auto i = 1; i < parms.Count(); i++)
                        {
                            if (newparms != "") newparms += ",";
                            newparms += parms[i].ToStdString();
                        }

                        p->SetCommandAtEndOfCurrentStep(parms[0].ToStdString(), newparms);
                    }
                }
            }
            else if (command == "Restart selected schedule")
            {
                auto rs = GetRunningSchedule();
                if (rs != nullptr)
                    rs->Reset();
            }
            else if (command == "Restart named schedule")
            {
                auto rs = GetRunningSchedule(parameters);
                if (rs != nullptr)
                    rs->Reset();
            }
            else if (command == "PressButton")
            {
                UserButton* b = _scheduleOptions->GetButton(parameters);

                if (b != nullptr)
                {
                    std::string c = b->GetCommand();
                    std::string p = b->GetParameters();

                    if (c != "")
                    {
                        result = Action(c, p, "", selplaylist, selschedule, rate, msg);
                    }
                }
                else
                {
                    result = false;
                    msg = "Unknown button.";
                }
            }
            else if (command == "Play specified playlist n times")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string pl = split[0].ToStdString();
                int loops = wxAtoi(split[1]);
                PlayList* p = GetPlayList(pl);

                if (p != nullptr)
                {
                    if (!PlayPlayList(p, rate, false, "", false, loops))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else if (command == "Refresh current playlist")
            {
                if (IsCurrentPlayListScheduled())
                {
                    auto rs = GetRunningSchedule();
                    auto plid = rs->GetPlayList()->GetId();
                    auto sid = rs->GetSchedule()->GetId();

                    bool loop = rs->GetPlayList()->IsLooping();
                    std::string step = "";
                    if (rs->GetPlayList()->GetRunningStep() != nullptr)
                    {
                        step = rs->GetPlayList()->GetRunningStep()->GetNameNoTime();
                    }
                    int loopsLeft = rs->GetPlayList()->GetLoopsLeft();
                    bool random = rs->GetPlayList()->IsRandom();

                    rs->GetPlayList()->Stop();
                    SendFPPSync("", 0xFFFFFFFF, 50);
                    _activeSchedules.remove(rs);
                    delete rs;

                    PlayList* orig = nullptr;
                    PlayList* pl = nullptr;
                    Schedule* sc = nullptr;

                    for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
                    {
                        if ((*it)->GetId() == plid)
                        {
                            orig = *it;
                            pl = new PlayList(**it);
                            break;
                        }
                    }

                    if (pl != nullptr)
                    {
                        auto schs = orig->GetSchedules();
                        for (auto it = schs.begin(); it != schs.end(); ++it)
                        {
                            if ((*it)->GetId() == sid)
                            {
                                sc = new  Schedule(**it);
                                break;
                            }
                        }

                        if (sc != nullptr)
                        {
                            rs = new RunningSchedule(pl, sc);
                            _activeSchedules.push_back(rs);
                            rs->GetPlayList()->StartSuspended(loop, random, loopsLeft, step);

                            _activeSchedules.sort(compare_runningschedules);
                        }
                    }

                    CheckSchedule();
                }
                else if (_immediatePlay != nullptr)
                {
                    PlayList* p = GetRunningPlayList();
                    if (p != nullptr)
                    {
                        bool loop = p->IsLooping();
                        bool forcelast = p->IsFinishingUp();
                        int loopsLeft = p->GetLoopsLeft();
                        bool random = p->IsRandom();

                        std::string step = p->GetRunningStep()->GetNameNoTime();
                        int steploopsleft = p->GetRunningStep()->GetLoopsLeft();

                        p->Stop();
                        SendFPPSync("", 0xFFFFFFFF, 50);

                        auto plid = p->GetId();

                        for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
                        {
                            if ((*it)->GetId() == plid)
                            {
                                PlayPlayList(*it, rate, loop, step, forcelast, loopsLeft, random, steploopsleft);
                                break;
                            }
                        }
                    }
                }
                else
                {
                    result = false;
                    msg = "Only scheduled and immediately played playlists can be restarted.";
                }
            }
            else if (command == "Bring to foreground")
            {
                ((wxFrame*)wxGetApp().GetTopWindow())->Iconize(false);
                wxGetApp().GetTopWindow()->Raise();
            }
            else if (command == "Set current text")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string textname = split[0].ToStdString();
                PlayListItemText* pliText = nullptr;
                PlayList* p = GetRunningPlayList();
                if (p != nullptr)
                {
                    pliText = p->GetRunningText(textname);
                }
                if (pliText == nullptr && _backgroundPlayList != nullptr)
                {
                    pliText = _backgroundPlayList->GetRunningText(textname);
                }

                if (pliText != nullptr)
                {
                    std::string text = "";
                    if (split.size() > 1)
                    {
                        text = split[1];
                    }

                    std::string properties = "";
                    if (split.size() > 2)
                    {
                        properties = split[2];
                    }

                    DoText(pliText, text, properties);
                }
                else
                {
                    result = false;
                    msg = "Text not found to set ... it may not be running.";
                }
            }
            else if (command == "Set pixels")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                size_t sc = wxAtol(split[0]);

                APPLYMETHOD blendMode = APPLYMETHOD::METHOD_OVERWRITE;
                if (split.size() > 1)
                {
                    blendMode = EncodeBlendMode(split[1].ToStdString());
                }

                PixelData * p = nullptr;
                for (auto it = _overlayData.begin(); it != _overlayData.end(); ++it)
                {
                    if ((*it)->GetStartChannel() == sc)
                    {
                        p = *it;
                        if (data.length() == 0)
                        {
                            _overlayData.remove(p);
                        }
                        else
                        {
                            p->SetData(data, blendMode);
                        }
                        break;
                    }
                }

                if (p == nullptr)
                {
                    p = new PixelData(sc, data, blendMode);
                    _overlayData.push_back(p);
                }
            }
            else if (command == "Play specified playlist step n times")
            {
                wxString parameter = parameters;
                wxArrayString split = wxSplit(parameter, ',');

                std::string pl = split[0].ToStdString();
                std::string step = split[1].ToStdString();
                int loops = wxAtoi(split[2]);

                PlayList* p = GetPlayList(pl);

                if (p != nullptr)
                {
                    if (!PlayPlayList(p, rate, false, step, false, -1, false, loops))
                    {
                        result = false;
                        msg = "Unable to start playlist.";
                    }
                }
            }
            else
            {
                result = false;
                msg = "Unrecognised command. Check command case.";
            }
        }
    }

    if (!result)
    {
        static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
        logger_base.error("Action failed: %s", (const char *)msg.c_str());

        wxCommandEvent event(EVT_STATUSMSG);
        event.SetString(msg);
        wxPostEvent(wxGetApp().GetTopWindow(), event);
    }

    // Clean up immediate play of one of the actions led to it stopping
    if (_immediatePlay != nullptr)
    {
        if (!_immediatePlay->IsRunning())
        {
            delete _immediatePlay;
            _immediatePlay = nullptr;
        }
    }

    wxCommandEvent event(EVT_SCHEDULECHANGED);
    wxPostEvent(wxGetApp().GetTopWindow(), event);

    return result;
}


bool ScheduleManager::Action(const std::string label, PlayList* selplaylist, Schedule* selschedule, size_t& rate, std::string& msg)
{
    UserButton* b = _scheduleOptions->GetButton(label);

    if (b != nullptr)
    {
        std::string command = b->GetCommand();
        std::string parameters = b->GetParameters();

        return Action(command, parameters, "", selplaylist, selschedule, rate, msg);
    }
    else
    {
        msg = "Unknown button.";
        return false;
    }
}

void ScheduleManager::StopPlayList(PlayList* playlist, bool atendofcurrentstep)
{
    if (_immediatePlay != nullptr && _immediatePlay->GetId() == playlist->GetId())
    {
        if (atendofcurrentstep)
        {
            _immediatePlay->StopAtEndOfCurrentStep();
        }
        else
        {
            SendFPPSync("", 0xFFFFFFFF, 50);
            _immediatePlay->Stop();
            delete _immediatePlay;
            _immediatePlay = nullptr;
        }
    }

    for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
    {
        if ((*it)->GetId() == playlist->GetId() && (*it)->IsRunning())
        {
            if (atendofcurrentstep)
            {
                (*it)->StopAtEndOfCurrentStep();
            }
            else
            {
                SendFPPSync("", 0xFFFFFFFF, 50);
                (*it)->Stop();
            }
        }
    }
}

// 127.0.0.1/xScheduleStash?Command=Store&Key=<key> ... this must be posted with the data in the body of the request ... key must be filename legal
// 127.0.0.1/xScheduleStash?Command=Retrieve&Key=<key> ... this returs a text response with the data if successful

// 127.0.0.1/xScheduleQuery?Query=GetPlayLists&Parameters=
// 127.0.0.1/xScheduleQuery?Query=GetPlayListSteps&Parameters=<playlistname>
// 127.0.0.1/xScheduleQuery?Query=GetPlayingStatus&Parameters=
// 127.0.0.1/xScheduleQuery?Query=GetButtons&Parameters=

bool ScheduleManager::Query(const std::string command, const std::string parameters, std::string& data, std::string& msg, const std::string& ip, const std::string& reference)
{
    bool result = true;
    data = "";
    if (command == "GetPlayLists")
    {
        data = "{\"playlists\":[";
        for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
        {
            if (it != _playLists.begin())
            {
                data += ",";
            }
            data += "{\"name\":\"" + (*it)->GetNameNoTime() +
                    "\",\"id\":\"" + wxString::Format(wxT("%i"), (*it)->GetId()).ToStdString() +
                "\",\"nextscheduled\":\"" + (*it)->GetNextScheduledTime() +
                "\",\"length\":\""+ FormatTime((*it)->GetLengthMS()) +"\"}";
        }
        data += "],\"reference\":\""+reference+"\"}";
    }
    else if (command == "GetPlayListSteps")
    {
        PlayList* p = GetPlayList(parameters);

        if (p != nullptr)
        {
            data = "{\"steps\":[";
            auto steps = p->GetSteps();
            for (auto it =  steps.begin(); it != steps.end(); ++it)
            {
                if (it != steps.begin())
                {
                    data += ",";
                }

                std::string first;
                if (*it == steps.front()  && p->GetFirstOnce())
                {
                    first = "\",\"startonly\":\"true";
                }
                else
                {
                    first = "\",\"startonly\":\"false";
                }

                std::string last;
                if (*it == steps.back() && p->GetLastOnce())
                {
                    last = "\",\"endonly\":\"true";
                }
                else
                {
                    last = "\",\"endonly\":\"false";
                }

                data += "{\"name\":\"" + (*it)->GetNameNoTime() +
                    "\",\"id\":\"" + wxString::Format(wxT("%i"), (*it)->GetId()).ToStdString() +
                    first + last +
                        "\",\"length\":\""+FormatTime((*it)->GetLengthMS())+"\"}";
            }
            data += "],\"reference\":\""+reference+"\"}";
        }
        else
        {
            data = "{\"steps\":[],\"reference\":\""+reference+"\"}";
            result = false;
            msg = "Playlist '" + parameters + "' not found.";
        }
    }
    else if (command == "GetMatrices")
    {
        data = "{\"matrices\":[";
        auto ms = _scheduleOptions->GetMatrices();
        for (auto it = ms->begin(); it != ms->end(); ++it)
        {
            if (it != ms->begin())
            {
                data += ",";
            }
            data += "\"" + (*it)->GetName() + "\"";
        }
        data += "],\"reference\":\""+reference+"\"}";
    }
    else if (command == "GetQueuedSteps")
    {
        PlayList* p = _queuedSongs;

        data = "{\"steps\":[";
        auto steps = p->GetSteps();
        for (auto it = steps.begin(); it != steps.end(); ++it)
        {
            if (it != steps.begin())
            {
                data += ",";
            }
            data += "{\"name\":\"" + (*it)->GetNameNoTime() +
                    "\",\"id\":\"" + wxString::Format(wxT("%i"), (*it)->GetId()).ToStdString() +
                    "\",\"length\":\"" + FormatTime((*it)->GetLengthMS()) + "\"}";
        }
        data += "],\"reference\":\""+reference+"\"}";
    }
    else if (command == "ListWebFolders")
    {
        if (wxString(parameters).Contains(".."))
        {
            result = false;
            msg = "Illegal request.";
        }
        else
        {
            // parameters holds the subdirectory to scan ... blank is the web directory

            wxString d;
#ifdef __WXMSW__
            d = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
#elif __LINUX__
            d = wxStandardPaths::Get().GetDataDir();
            if (!wxDir::Exists(d)) {
                d = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
            }
#else
            d = wxStandardPaths::Get().GetResourcesDir();
#endif
            d += "/" + _scheduleOptions->GetWWWRoot();

            if (parameters != "")
            {
                d += "/" + parameters;
            }

            if (!wxDir::Exists(d))
            {
                result = false;
                msg = "Unknown folder.";
            }
            else
            {
                wxDir dir(d);

                data = "{\"folders\":[";
                bool first = true;
                wxString dirname;
                bool found = dir.GetFirst(&dirname, "", wxDIR_DIRS);

                while (found)
                {
                    if (!first)
                    {
                        data += ",";
                    }
                    first = false;
                    data += "\"" + dirname + "\"";

                    found = dir.GetNext(&dirname);
                }
                data += "],\"reference\":\""+reference+"\"}";
            }
        }
    }
    else if (command == "GetNextScheduledPlayList")
    {
        PlayList* p = nullptr;
        Schedule* s = nullptr;;
        wxDateTime next = wxDateTime((time_t)0);
        for (auto pit = _playLists.begin() ; pit != _playLists.end(); ++pit)
        {
            auto schedules = (*pit)->GetSchedules();
            for (auto sit = schedules.begin(); sit != schedules.end(); ++sit)
            {
                wxDateTime n = (*sit)->GetNextTriggerDateTime();
                if (n != wxDateTime((time_t)0))
                {
                    if (next == wxDateTime((time_t)0))
                    {
                        p = *pit;
                        s = *sit;
                        next = n;
                    }
                    else if (n < next)
                    {
                        p = *pit;
                        s = *sit;
                        next = n;
                    }
                }
            }
        }

        if (p == nullptr)
        {
            data = "{\"playlistname\":\"\",\"playlistid\":\"\",\"schedulename\":\"\",\"scheduleid\":\"\",\"start\":\"Never\",\"end\":\"\",\"reference\":\"" + reference + "\"}";
        }
        else
        {
            data = "{\"playlistname\":\"" + p->GetNameNoTime() + "\"," +
                "\"playlistid\":\"" + wxString::Format("%i", p->GetId()).ToStdString() + "\"," +
                "\"schedulename\":\"" + s->GetName() + "\"," +
                "\"scheduleid\":\"" + wxString::Format("%i", s->GetId()).ToStdString() + "\"," +
                "\"start\":\"" + s->GetNextTriggerTime() + "\"," +
                "\"end\":\"" + s->GetEndTimeAsString() + "\"," +
                "\"reference\":\"" + reference + "\"" +
                "}";
        }
    }
    else if (command == "GetPlayListSchedules")
    {
        PlayList* p = GetPlayList(parameters);
        if (p != nullptr)
        {
            data = "{\"schedules\":[";
            auto scheds = p->GetSchedules();
            for (auto it = scheds.begin(); it != scheds.end(); ++it)
            {
                if (it != scheds.begin())
                {
                    data += ",";
                }
                data += (*it)->GetJSON("");
            }
            data += "],\"reference\":\""+reference+"\"}";
        }
        else
        {
            data = "{\"schedules\":[],\"reference\":\""+reference+"\"}";
            result = false;
            msg = "Playlist '" + parameters + "' not found.";
        }
    }
    else if (command == "GetPlayListSchedule")
    {
        wxArrayString plsc = wxSplit(parameters, ',');

        if (plsc.Count() == 2)
        {
            PlayList* p = GetPlayList(plsc[0].ToStdString());
            if (p != nullptr)
            {
                Schedule* schedule = p->GetSchedule(plsc[1].ToStdString());

                if (schedule != nullptr)
                {
                    data = schedule->GetJSON(reference);
                }
                else
                {
                    result = false;
                    msg = "Playlist '" + plsc[0] + "' does not have a schedule '"+plsc[1]+"'.";
                }
            }
            else
            {
                result = false;
                msg = "Playlist '" + plsc[0] + "' not found.";
            }
        }
        else
        {
            result = false;
            msg = "Incorrect parameters. Playlist and schedule expected: " + parameters;
        }
    }
    else if (command == "GetPlayingStatus")
    {
        PlayList* p = GetRunningPlayList();
        if (p == nullptr || p->GetRunningStep() == nullptr)
        {
            data = "{\"status\":\"idle\",\"outputtolights\":\"" + std::string(_outputManager->IsOutputting() ? "true" : "false") +
                "\",\"volume\":\"" + wxString::Format(wxT("%i"), GetVolume()) +
                "\",\"ip\":\"" + ip +
                "\",\"reference\":\"" + reference +
                "\",\"time\":\""+ wxDateTime::Now().Format("%Y-%m-%d %H:%M:%S") +"\"}";
        }
        else
        {
            std::string nextsong;
            std::string nextsongid;
            bool didloop;
            auto next = p->GetNextStep(didloop);
            if (next == nullptr)
            {
                nextsong = "";
                nextsongid = "";
            }
            else if (p->IsRandom())
            {
                nextsong = "God knows";
                nextsongid = "";
            }
            else
            {
                nextsong = next->GetNameNoTime();
                nextsongid = wxString::Format(wxT("%i"), next->GetId());
            }

            RunningSchedule* rs = GetRunningSchedule();

            data = "{\"status\":\"" + std::string(p->IsPaused() ? "paused" : "playing") +
                "\",\"playlist\":\"" + p->GetNameNoTime() +
                "\",\"playlistid\":\"" + wxString::Format(wxT("%i"), p->GetId()).ToStdString() +
                "\",\"playlistlooping\":\"" + (p->IsLooping() || p->GetLoopsLeft() > 0 ? "true" : "false") +
                "\",\"playlistloopsleft\":\"" + wxString::Format(wxT("%i"),p->GetLoopsLeft()).ToStdString() +
                "\",\"random\":\"" + (p->IsRandom() ? "true" : "false") +
                "\",\"step\":\"" + p->GetRunningStep()->GetNameNoTime() +
                "\",\"stepid\":\"" + wxString::Format(wxT("%i"), p->GetRunningStep()->GetId()).ToStdString() +
                "\",\"steplooping\":\"" + (p->IsStepLooping() || p->GetRunningStep()->GetLoopsLeft() > 0 ? "true" : "false") +
                "\",\"steploopsleft\":\"" + wxString::Format(wxT("%i"), p->GetRunningStep()->GetLoopsLeft()).ToStdString() +
                "\",\"length\":\"" + FormatTime(p->GetRunningStep()->GetLengthMS()) +
                "\",\"position\":\"" + FormatTime(p->GetRunningStep()->GetPosition()) +
                "\",\"left\":\"" + FormatTime(p->GetRunningStep()->GetLengthMS() - p->GetRunningStep()->GetPosition()) +
                "\",\"trigger\":\"" + std::string(IsCurrentPlayListScheduled() ? "scheduled": (_immediatePlay != nullptr) ? "manual" : "queued") +
                "\",\"schedulename\":\"" + std::string((IsCurrentPlayListScheduled() && rs != nullptr) ? rs->GetSchedule()->GetName() : "N/A") +
                "\",\"scheduleend\":\"" + std::string((IsCurrentPlayListScheduled() && rs != nullptr) ? rs->GetSchedule()->GetNextEndTime() : "N/A") +
                "\",\"scheduleid\":\"" + std::string((IsCurrentPlayListScheduled() && rs != nullptr) ? wxString::Format(wxT("%i"), rs->GetSchedule()->GetId()).ToStdString()  : "N/A") +
                "\",\"nextstep\":\"" + nextsong +
                "\",\"nextstepid\":\"" + nextsongid +
                "\",\"version\":\"" + xlights_version_string +
                "\",\"queuelength\":\"" + wxString::Format(wxT("%i"), (long)_queuedSongs->GetSteps().size()) +
                "\",\"volume\":\"" + wxString::Format(wxT("%i"), GetVolume()) +
                "\",\"time\":\"" + wxDateTime::Now().Format("%Y-%m-%d %H:%M:%S") +
                "\",\"ip\":\"" + ip +
                "\",\"reference\":\"" + reference +
                "\",\"outputtolights\":\"" + std::string(_outputManager->IsOutputting() ? "true" : "false") + "\"}";
            //static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
            //logger_base.info("%s", (const char*)data.c_str());
        }
    }
    else if (command == "GetButtons")
    {
        data = _scheduleOptions->GetButtonsJSON(_commandManager, reference);
    }
    else
    {
        result = false;
        msg = "Unknown query.";
    }

    return result;
}

bool ScheduleManager::StoreData(const std::string& key, const std::string& data, std::string& msg) const
{
    bool result = true;

    if (!wxDir::Exists(_showDir + "/xScheduleData"))
    {
        wxDir sd(_showDir);
        sd.Make(_showDir + "/xScheduleData");
    }

    std::string filename = _showDir + "/xScheduleData/" + key + ".dat";

    wxFile dataFile(filename, wxFile::write);

    if (dataFile.IsOpened())
    {
        dataFile.Write(data.c_str(), data.size());
    }
    else
    {
        result = false;
        msg = "Unable to create file " + filename;
    }

    return result;
}

bool ScheduleManager::RetrieveData(const std::string& key, std::string& data, std::string& msg) const
{
    bool result = true;

    std::string filename = _showDir + "/xScheduleData/" + key + ".dat";

    if (!wxFile::Exists(filename))
    {
        data = "";
        result = false;
    }
    else
    {
        wxFile dataFile(filename);

        wxString d = "";
        dataFile.ReadAll(&d);
        data = d.ToStdString();
    }

    return result;
}

bool ScheduleManager::ToggleCurrentPlayListRandom(std::string& msg)
{
    PlayList* p = GetRunningPlayList();

    if (p != nullptr)
    {
        return p->SetRandom(!p->IsRandom());
    }
    else
    {
        msg = "No playlist currently playing.";
        return false;
    }
}

bool ScheduleManager::ToggleCurrentPlayListPause(std::string& msg)
{
    PlayList* p = GetRunningPlayList();
    if (p != nullptr)
    {
        p->Pause();
    }
    else
    {
        msg = "No playlist currently playing.";
        return false;
    }

    return true;
}

bool ScheduleManager::ToggleCurrentPlayListLoop(std::string& msg)
{
    PlayList* p = GetRunningPlayList();

    if (p != nullptr)
    {
        return p->SetLooping(!p->IsLooping());
    }
    else
    {
        msg = "No playlist currently playing.";
        return false;
    }
}

bool ScheduleManager::ToggleCurrentPlayListStepLoop(std::string& msg)
{
    PlayList* p = GetRunningPlayList();

    if (p != nullptr)
    {
        if (p->IsStepLooping())
        {
            p->ClearStepLooping();
        }
        else
        {
            if (!p->LoopStep(p->GetRunningStep()->GetNameNoTime()))
            {
                msg = "Unable to loop the current step.";
                return false;
            }
        }
        return true;
    }
    else
    {
        msg = "No playlist currently playing.";
        return false;
    }
}

bool ScheduleManager::IsOutputToLights() const
{
    return _outputManager != nullptr && _outputManager->IsOutputting();
}

RunningSchedule* ScheduleManager::GetRunningSchedule() const
{
    if (_immediatePlay != nullptr) return nullptr;
    if (_queuedSongs->IsRunning()) return nullptr;
    if (_activeSchedules.size() == 0) return nullptr;

    for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
    {
        if ((*it)->GetPlayList()->IsRunning())
        {
            return *it;
        }
    }

    return nullptr;
}

bool ScheduleManager::IsScheduleActive(Schedule* schedule)
{
    for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
    {
        if ((*it)->GetSchedule()->GetId() == schedule->GetId()) return true;
    }

    return false;
}

RunningSchedule* ScheduleManager::GetRunningSchedule(Schedule* schedule) const
{
    for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
    {
        if ((*it)->GetSchedule()->GetId() == schedule->GetId()) return *it;
    }

    return nullptr;
}

RunningSchedule* ScheduleManager::GetRunningSchedule(const std::string& schedulename) const
{
    for (auto it = _activeSchedules.begin(); it != _activeSchedules.end(); ++it)
    {
        if (wxString((*it)->GetSchedule()->GetName()).Lower() == wxString(schedulename).Lower()) return *it;
    }

    return nullptr;
}

void ScheduleManager::SetOutputToLights(bool otl)
{
    if (_outputManager != nullptr)
    {
        if (otl)
        {
            if (!IsOutputToLights())
            {
                _outputManager->StartOutput();
                StartVirtualMatrices();
                ManageBackground();
            }
        }
        else
        {
            if (IsOutputToLights())
            {
                _outputManager->StopOutput();
                StopVirtualMatrices();
                ManageBackground();
            }
        }
    }
}

void ScheduleManager::ManualOutputToLightsClick()
{
    _manualOTL++;
    if (_manualOTL > 1) _manualOTL = -1;
    if (_manualOTL == 1)
    {
        _outputManager->StartOutput();
        StartVirtualMatrices();
        ManageBackground();
    }
    else if (_manualOTL == 0)
    {
        _outputManager->StopOutput();
        StopVirtualMatrices();
        ManageBackground();
    }
}

bool ScheduleManager::ToggleOutputToLights(std::string& msg)
{
    if (_outputManager->IsOutputting())
    {
        _manualOTL = 0;
        SetOutputToLights(false);
    }
    else
    {
        _manualOTL = 1;
        SetOutputToLights(true);
    }

    return true;
}

void ScheduleManager::SetVolume(int volume)
{
    int cv = volume;
    if (cv < 0) cv = 0;
    if (cv > 100) cv = 100;
    AudioManager::SetGlobalVolume(cv);
}
void ScheduleManager::AdjustVolumeBy(int volume)
{
    int cv = GetVolume();
    cv += volume;
    SetVolume(cv);
}
int ScheduleManager::GetVolume() const
{
    return AudioManager::GetGlobalVolume();
}

size_t ScheduleManager::GetTotalChannels() const
{
    if (_outputManager != nullptr)
        return _outputManager->GetTotalChannels();

    return 0;
}

void ScheduleManager::ToggleMute()
{
    static int savevolume = 100;
    if (GetVolume() > 0)
    {
        savevolume = GetVolume();
        SetVolume(0);
    }
    else
    {
        SetVolume(savevolume);
    }
}

void ScheduleManager::SetMode(SYNCMODE mode)
{
    if (_mode != mode)
    {
        _mode = mode;

        wxConfigBase* config = wxConfigBase::Get();
        config->Write("SyncMode", (long)_mode);

        if (_mode == SYNCMODE::FPPMASTER)
        {
            OpenFPPSyncSendSocket();
            CloseFPPSyncListenSocket();
        }
        else if (_mode == SYNCMODE::FPPSLAVE)
        {
            CloseFPPSyncSendSocket();
            OpenFPPSyncListenSocket();
        }
        else
        {
            CloseFPPSyncSendSocket();
            CloseFPPSyncListenSocket();
        }
    }
}

PlayList* ScheduleManager::GetPlayList(int id) const
{
    for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
    {
        if ((*it)->GetId() == id) return *it;
    }

    return nullptr;
}

void ScheduleManager::SetBackgroundPlayList(PlayList* playlist)
{
    if (playlist == nullptr && _backgroundPlayList != nullptr)
    {
        _backgroundPlayList->Stop();
        delete _backgroundPlayList;
        _backgroundPlayList = nullptr;
        _changeCount++;
    }
    else if (playlist != nullptr)
    {
        if (_backgroundPlayList == nullptr)
        {
            _backgroundPlayList = new PlayList(*playlist);
            _changeCount++;
        }
        else
        {
            if (playlist->GetId() != _backgroundPlayList->GetId())
            {
                _backgroundPlayList->Stop();
                delete _backgroundPlayList;
                _backgroundPlayList = new PlayList(*playlist);
                _changeCount++;
            }
        }
    }
}

void ScheduleManager::ManageBackground()
{
    if (_backgroundPlayList != nullptr)
    {
        if (IsOutputToLights())
        {
            _backgroundPlayList->Stop();
            int id = _backgroundPlayList->GetId();
            delete _backgroundPlayList;
            _backgroundPlayList = nullptr;
            PlayList* pl = GetPlayList(id);
            if (pl != nullptr)
            {
                _backgroundPlayList = new PlayList(*pl);
                _backgroundPlayList->Start(true);
            }
        }
        else
        {
            _backgroundPlayList->Stop();
        }
    }
}

void LogAndWrite(wxFile& f, const std::string& msg)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    logger_base.debug("CheckSchedule: " + msg);
    if (f.IsOpened())
    {
        f.Write(msg + "\r\n");
    }
}

void ScheduleManager::CheckScheduleIntegrity(bool display)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    int errcount = 0;
    int warncount = 0;
    int errcountsave = 0;
    int warncountsave = 0;

    wxFile f;
    wxString filename = wxFileName::CreateTempFileName("xLightsCheckSequence") + ".txt";

    if (display)
    {
        f.Open(filename, wxFile::write);
        if (!f.IsOpened())
        {
            logger_base.warn("Unable to create results file for Check Sequence. Aborted.");
            wxMessageBox(_("Unable to create results file for Check Sequence. Aborted."), _("Error"));
            return;
        }
    }

    LogAndWrite(f, "Checking schedule.");

    wxIPV4address addr;
    addr.Hostname(wxGetHostName());
    LogAndWrite(f, "");
    LogAndWrite(f, "IP Address we are outputing data from: " + addr.IPAddress().ToStdString());
    LogAndWrite(f, "If your PC has multiple network connections (such as wired and wireless) this should be the IP Address of the adapter your controllers are connected to. If it isnt your controllers may not receive output data.");
    LogAndWrite(f, "If you are experiencing this problem you may need to disable this network connection.");

    LogAndWrite(f, "");
    LogAndWrite(f, "Inactive Outputs");

    // Check for inactive outputs
    auto outputs = _outputManager->GetOutputs();
    for (auto it = outputs.begin(); it != outputs.end(); ++it)
    {
        if (!(*it)->IsEnabled())
        {
            wxString msg = wxString::Format("    WARN: Inactive output %d %s:%s:%s:%s:'%s'.",
                (*it)->GetOutputNumber(), (*it)->GetType(), (*it)->GetIP(), (*it)->GetUniverseString(), (*it)->GetCommPort(), (*it)->GetDescription());
            LogAndWrite(f, msg.ToStdString());
            warncount++;
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // multiple outputs to same universe and same IP
    LogAndWrite(f, "");
    LogAndWrite(f, "Multiple outputs sending to same destination");

    std::list<std::string> used;
    outputs = _outputManager->GetOutputs();
    for (auto n = outputs.begin(); n != outputs.end(); ++n)
    {
        if ((*n)->IsIpOutput())
        {
            std::string usedval = (*n)->GetIP() + "|" + (*n)->GetUniverseString();

            if (std::find(used.begin(), used.end(), usedval) != used.end())
            {
                wxString msg = wxString::Format("    ERR: Multiple outputs being sent to the same controller '%s' (%s) and universe %s.", (const char*)(*n)->GetDescription().c_str(), (const char*)(*n)->GetIP().c_str(), (const char *)(*n)->GetUniverseString().c_str());
                LogAndWrite(f, msg.ToStdString());
                errcount++;
            }
            else
            {
                used.push_back(usedval);
            }
        }
        else if ((*n)->IsSerialOutput())
        {
            if (std::find(used.begin(), used.end(), (*n)->GetCommPort()) != used.end())
            {
                wxString msg = wxString::Format("    ERR: Multiple outputs being sent to the same comm port %s '%s' %s.", (const char *)(*n)->GetType().c_str(), (const char *)(*n)->GetCommPort().c_str(), (const char*)(*n)->GetDescription().c_str());
                LogAndWrite(f, msg.ToStdString());
                errcount++;
            }
            else
            {
                used.push_back((*n)->GetCommPort());
            }
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // duplicate playlist names
    LogAndWrite(f, "");
    LogAndWrite(f, "Duplicate playlist names");

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        auto n1 = n;
        n1++;

        while (n1 != _playLists.end())
        {
            if ((*n1)->GetNameNoTime() == (*n)->GetNameNoTime())
            {
                wxString msg = wxString::Format("    ERR: Multiple PlayLists named '%s'. Commands which rely on playlist name may pick up the wrong one.", (const char*)(*n)->GetNameNoTime().c_str());
                LogAndWrite(f, msg.ToStdString());
                errcount++;
            }

            n1++;
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Duplicate step names within a playlist
    LogAndWrite(f, "");
    LogAndWrite(f, "Duplicate step names within a playlist");

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        auto steps = (*n)->GetSteps();
        for (auto s = steps.begin(); s != steps.end(); ++s)
        {
            auto s1 = s;
            ++s1;

            while (s1 != steps.end())
            {
                if ((*s1)->GetNameNoTime() == (*s)->GetNameNoTime())
                {
                    wxString msg = wxString::Format("    ERR: Multiple playlists steps named '%s' exist in playlist '%s'. Commands which rely on step name may pick up the wrong one.", (const char *)(*s)->GetNameNoTime().c_str(), (const char*)(*n)->GetNameNoTime().c_str());
                    LogAndWrite(f, msg.ToStdString());
                    errcount++;
                }

                ++s1;
            }
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Duplicate schedule names within a playlist
    LogAndWrite(f, "");
    LogAndWrite(f, "Duplicate schedule names within a playlist");

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        auto scheds = (*n)->GetSchedules();
        for (auto s = scheds.begin(); s != scheds.end(); ++s)
        {
            auto s1 = s;
            ++s1;

            while (s1 != scheds.end())
            {
                if ((*s1)->GetName() == (*s)->GetName())
                {
                    wxString msg = wxString::Format("    ERR: Multiple playlists schedules named '%s' exist in playlist '%s'. Commands which rely on schedule name may pick up the wrong one.", (const char *)(*s)->GetName().c_str(), (const char*)(*n)->GetNameNoTime().c_str());
                    LogAndWrite(f, msg.ToStdString());
                    errcount++;
                }

                ++s1;
            }
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Empty Playlists
    LogAndWrite(f, "");
    LogAndWrite(f, "Empty playlists");

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        if ((*n)->GetSteps().size() == 0)
        {
            wxString msg = wxString::Format("    WARN: Playlist '%s' has no steps.", (const char*)(*n)->GetNameNoTime().c_str());
            LogAndWrite(f, msg.ToStdString());
            warncount++;
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Empty Steps
    LogAndWrite(f, "");
    LogAndWrite(f, "Empty steps");

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        auto steps = (*n)->GetSteps();
        for (auto s = steps.begin(); s != steps.end(); ++s)
        {
            if ((*s)->GetItems().size() == 0)
            {
                wxString msg = wxString::Format("    WARN: Playlist '%s' has step '%s' with no items.", (const char*)(*n)->GetNameNoTime().c_str(), (const char*)(*s)->GetNameNoTime().c_str());
                LogAndWrite(f, msg.ToStdString());
                warncount++;
            }
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Multiple FSEQ in one step
    LogAndWrite(f, "");
    LogAndWrite(f, "Steps with multiple FSEQs");

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        auto steps = (*n)->GetSteps();
        for (auto s = steps.begin(); s != steps.end(); ++s)
        {
            int fseqcount = 0;

            auto items = (*s)->GetItems();

            for (auto i  = items.begin(); i != items.end(); ++i)
            {
                if (wxString((*i)->GetTitle()).Contains("FSEQ"))
                {
                    fseqcount++;
                }
            }

            if (fseqcount > 1)
            {
                wxString msg = wxString::Format("    WARN: Playlist '%s' has step '%s' with more than one FSEQ item.", (const char*)(*n)->GetNameNoTime().c_str(), (const char*)(*s)->GetNameNoTime().c_str());
                LogAndWrite(f, msg.ToStdString());
                warncount++;
            }
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Multiple Audio in one step
    LogAndWrite(f, "");
    LogAndWrite(f, "Steps with multiple audio");

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        auto steps = (*n)->GetSteps();
        for (auto s = steps.begin(); s != steps.end(); ++s)
        {
            int audiocount = 0;

            auto items = (*s)->GetItems();

            for (auto i = items.begin(); i != items.end(); ++i)
            {
                if ((*i)->GetTitle() == "Audio" ||
                    ((*i)->GetTitle() == "FSEQ" && ((PlayListItemFSEQ*)(*i))->GetAudioFilename() != "") ||
                    ((*i)->GetTitle() == "FSEQ & Video" && ((PlayListItemFSEQVideo*)(*i))->GetAudioFilename() != "")
                    )
                {
                    audiocount++;
                }
            }

            if (audiocount > 1)
            {
                wxString msg = wxString::Format("    WARN: Playlist '%s' has step '%s' with more than one item playing audio.", (const char*)(*n)->GetNameNoTime().c_str(), (const char*)(*s)->GetNameNoTime().c_str());
                LogAndWrite(f, msg.ToStdString());
                warncount++;
            }
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Excessive video in one step
    LogAndWrite(f, "");
    LogAndWrite(f, "Steps with many videos");

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        auto steps = (*n)->GetSteps();
        for (auto s = steps.begin(); s != steps.end(); ++s)
        {
            int videocount = 0;

            auto items = (*s)->GetItems();

            for (auto i = items.begin(); i != items.end(); ++i)
            {
                if (wxString((*i)->GetTitle()).Contains("Video"))
                {
                    videocount++;
                }
            }

            if (videocount > 3)
            {
                wxString msg = wxString::Format("    WARN: Playlist '%s' has step '%s' with more than 3 videos ... this can cause performance issues.", (const char*)(*n)->GetNameNoTime().c_str(), (const char*)(*s)->GetNameNoTime().c_str());
                LogAndWrite(f, msg.ToStdString());
                warncount++;
            }
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Duplicate matrix names
    LogAndWrite(f, "");
    LogAndWrite(f, "Duplicate matrix names");

    used.clear();
    auto m = GetOptions()->GetMatrices();
    for (auto n = m->begin(); n != m->end(); ++n)
    {
        if (std::find(used.begin(), used.end(), (*n)->GetName()) == used.end())
        {
            used.push_back((*n)->GetName());
        }
        else
        {
            wxString msg = wxString::Format("    ERR: Duplicate matrix '%s'.", (const char*)(*n)->GetName().c_str());
            LogAndWrite(f, msg.ToStdString());
            errcount++;
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Matrices greater than available channels
    LogAndWrite(f, "");
    LogAndWrite(f, "Matrices greater than available channels");

    m = GetOptions()->GetMatrices();
    for (auto n = m->begin(); n != m->end(); ++n)
    {
        if ((*n)->GetStartChannel() + (*n)->GetChannels() >= _outputManager->GetTotalChannels())
        {
            wxString msg = wxString::Format("    ERR: Matrix '%s' is meant to finish at channel %ld but last available channel is %ld.", (const char*)(*n)->GetName().c_str(), (long)((*n)->GetStartChannel() + (*n)->GetChannels()), (long)_outputManager->GetTotalChannels());
            LogAndWrite(f, msg.ToStdString());
            errcount++;
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Virtual Matrices greater than available channels
    LogAndWrite(f, "");
    LogAndWrite(f, "Virtual matrices greater than available channels");

    auto vm = GetOptions()->GetVirtualMatrices();
    for (auto n = vm->begin(); n != vm->end(); ++n)
    {
        if ((*n)->GetStartChannel() + (*n)->GetChannels() >= _outputManager->GetTotalChannels())
        {
            wxString msg = wxString::Format("    ERR: Virtual Matrix '%s' is meant to finish at channel %ld but last available channel is %ld.", (const char*)(*n)->GetName().c_str(), (long)((*n)->GetStartChannel() + (*n)->GetChannels()), (long)_outputManager->GetTotalChannels());
            LogAndWrite(f, msg.ToStdString());
            errcount++;
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Duplicate virtual matrix names
    LogAndWrite(f, "");
    LogAndWrite(f, "Duplicate virtual matrix names");

    used.clear();
    vm = GetOptions()->GetVirtualMatrices();
    for (auto n = vm->begin(); n != vm->end(); ++n)
    {
        if (std::find(used.begin(), used.end(), (*n)->GetName()) == used.end())
        {
            used.push_back((*n)->GetName());
        }
        else
        {
            wxString msg = wxString::Format("    ERR: Duplicate virtual matrix '%s'.", (const char*)(*n)->GetName().c_str());
            LogAndWrite(f, msg.ToStdString());
            errcount++;
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Multiple schedules with the same priority
    LogAndWrite(f, "");
    LogAndWrite(f, "Multiple schedules with the same priority");

    std::vector<int> priorities;
    priorities.resize(20);

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        auto scheds = (*n)->GetSchedules();
        for (auto s = scheds.begin(); s != scheds.end(); ++s)
        {
            priorities[(*s)->GetPriority()]++;
        }
    }

    for (int i = 0; i < 20; i++)
    {
        if (priorities[i] > 1)
        {
            wxString msg = wxString::Format("    WARN: More than one schedule has priority %d. If these trigger at the same time then it is not certain which we will choose.", i);
            LogAndWrite(f, msg.ToStdString());
            warncount++;
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // No web password
    LogAndWrite(f, "");
    LogAndWrite(f, "No password set on website");

    if (_scheduleOptions->GetPassword() == "")
    {
            wxString msg = wxString::Format("    WARN: Website does not have a password set.");
            LogAndWrite(f, msg.ToStdString());
            warncount++;
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Non standard web port
    LogAndWrite(f, "");
    LogAndWrite(f, "Non standard web port");

    if (_scheduleOptions->GetWebServerPort() != 80)
    {
        wxString msg = wxString::Format("    WARN: Website is listening on a non standard port %d.", _scheduleOptions->GetWebServerPort());
        LogAndWrite(f, msg.ToStdString());
        warncount++;
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // API Only mode
    LogAndWrite(f, "");
    LogAndWrite(f, "Web in API only mode");

    if (_scheduleOptions->GetAPIOnly())
    {
        wxString msg = wxString::Format("    WARN: Website is listening only for API calls. It will not allow connection from a browser.");
        LogAndWrite(f, msg.ToStdString());
        warncount++;
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Web server folder blank
    LogAndWrite(f, "");
    LogAndWrite(f, "Web server folder blank");

    if (_scheduleOptions->GetWWWRoot() == "")
    {
        wxString msg = wxString::Format("    ERR: Website folder is set to blank.");
        LogAndWrite(f, msg.ToStdString());
        errcount++;
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Missing index.html
    LogAndWrite(f, "");
    LogAndWrite(f, "Missing index.html");

    wxString d;
#ifdef __WXMSW__
    d = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
#elif __LINUX__
    d = wxStandardPaths::Get().GetDataDir();
    if (!wxDir::Exists(d)) {
        d = wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath();
    }
#else
    d = wxStandardPaths::Get().GetResourcesDir();
#endif

    wxString file = d + "/" + _scheduleOptions->GetWWWRoot() + "/index.html";

    if (_scheduleOptions->GetWWWRoot() != "" && !wxFile::Exists(file))
    {
        wxString msg = wxString::Format("    ERR: index.html not found. It should be here '%s'.", (const char *)file.c_str());
        LogAndWrite(f, msg.ToStdString());
        errcount++;
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }
    errcountsave = errcount;
    warncountsave = warncount;

    // Missing referenced files
    LogAndWrite(f, "");
    LogAndWrite(f, "Missing referenced files");

    for (auto n = _playLists.begin(); n != _playLists.end(); ++n)
    {
        auto steps = (*n)->GetSteps();
        for (auto s = steps.begin(); s != steps.end(); ++s)
        {
            auto items = (*s)->GetItems();

            for (auto i = items.begin(); i != items.end(); ++i)
            {
                auto missing = (*i)->GetMissingFiles();

                for (auto ff= missing.begin(); ff != missing.end(); ++ff)
                {
                    wxString msg = wxString::Format("    ERR: Playlist '%s' step '%s' item '%s' references file '%s' which does not exist.", (const char*)(*n)->GetNameNoTime().c_str(), (const char*)(*s)->GetNameNoTime().c_str(), (const char*)(*i)->GetNameNoTime().c_str(), (const char *)(ff->c_str()));
                    LogAndWrite(f, msg.ToStdString());
                    errcount++;
                }
            }
        }
    }

    if (errcount + warncount == errcountsave + warncountsave)
    {
        LogAndWrite(f, "    No problems found");
    }

    LogAndWrite(f, "");
    LogAndWrite(f, "Check schedule done.");
    LogAndWrite(f, wxString::Format("Errors: %d. Warnings: %d", errcount, warncount).ToStdString());

    if (f.IsOpened())
    {
        f.Close();

        wxFileType *ft = wxTheMimeTypesManager->GetFileTypeFromExtension("txt");
        if (ft)
        {
            wxString command = ft->GetOpenCommand(filename);

            logger_base.debug("Viewing xSchedule Check Schedule results %s.", (const char *)filename.c_str());

            wxExecute(command);
            delete ft;
        }
        else
        {
            logger_base.warn("Unable to view xSchedule Check Schedule results %s.", (const char *)filename.c_str());
            wxMessageBox(_("Unable to show xSchedule Check Schedule results."), _("Error"));
        }
    }
}

void ScheduleManager::ImportxLightsSchedule(const std::string& filename)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    wxFileName fn(filename);
    std::string base = fn.GetPath().ToStdString();

    wxXmlDocument doc;
    doc.Load(filename);

    if (doc.IsOk())
    {
        for (wxXmlNode* n = doc.GetRoot()->GetChildren(); n != nullptr; n = n->GetNext())
        {
            if (n->GetName() == "playlists")
            {
                for (wxXmlNode* n1 = n->GetChildren(); n1 != nullptr; n1 = n1->GetNext())
                {
                    if (n1->GetName() == "playlist")
                    {
                        std::string name = n1->GetAttribute("name", "").ToStdString();
                        PlayList* p = new PlayList();
                        p->SetName(name);
                        for (wxXmlNode* n2 = n1->GetChildren(); n2 != nullptr; n2 = n2->GetNext())
                        {
                            if (n2->GetName() == "listitem")
                            {
                                std::string itemname = n2->GetAttribute("name", "").ToStdString();
                                int delay = wxAtoi(n2->GetAttribute("delay", "0"));
                                PlayListStep* step = new PlayListStep();
                                step->SetName(itemname);
                                std::string ext = wxFileName(itemname).GetExt().Lower().ToStdString();
                                if (PlayListItemAudio::IsAudio(ext))
                                {
                                    PlayListItemAudio* pli = new PlayListItemAudio();
                                    pli->SetAudioFile(base + "/" + itemname);
                                    pli->SetDelay(delay * 1000);
                                    step->AddItem(pli);
                                }
                                else if (PlayListItemVideo::IsVideo(ext))
                                {
                                    PlayListItemVideo* pli = new PlayListItemVideo();
                                    pli->SetVideoFile(base + "/" + itemname);
                                    pli->SetDelay(delay * 1000);
                                    step->AddItem(pli);
                                }
                                else if (ext == "fseq")
                                {
                                    PlayListItemFSEQ* pli = new PlayListItemFSEQ();
                                    pli->SetFSEQFileName(base + "/" + itemname);
                                    pli->SetDelay(delay * 1000);
                                    step->AddItem(pli);
                                }
                                p->AddStep(step, p->GetSteps().size());
                            }
                        }
                        AddPlayList(p);
                    }
                }
            }
        }
    }
    else
    {
        logger_base.error("Invalid xLights schedule file.");
        wxMessageBox("Invalid xLights schedule file.");
    }
}

bool ScheduleManager::DoXyzzy(const std::string& command, const std::string& parameters, std::string& result, const std::string& reference)
{
    _lastXyzzyCommand = wxDateTime::Now();

    if (_xyzzy == nullptr)
    {
        _xyzzy = new Xyzzy();
        wxCommandEvent event(EVT_SCHEDULECHANGED);
        wxPostEvent(wxGetApp().GetTopWindow(), event);
    }

    if (command == "initialise")
    {
        _xyzzy->Initialise(parameters, result, reference);
    }
    else if (command == "close")
    {
        _xyzzy->Close(result, reference);
        delete _xyzzy;
        _xyzzy = nullptr;
        wxCommandEvent event(EVT_SCHEDULECHANGED);
        wxPostEvent(wxGetApp().GetTopWindow(), event);
    }
    else
    {
        _xyzzy->Action(command, parameters, result, reference);
    }

    if (_xyzzy != nullptr && !_xyzzy->IsOk())
    {
        delete _xyzzy;
        _xyzzy = nullptr;
    }

    return true;
}

static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static inline bool is_base64(unsigned char c)
{
    return (isalnum(c) || (c == '+') || (c == '/'));
}

//returns number of chars at the end that couldn't be decoded
static int base64_decode(const wxString& encoded_string, std::vector<unsigned char> &data)
{
    size_t in_len = encoded_string.size();
    int i = 0;
    int j = 0;
    int in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];

    while (in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_]))
    {
        char_array_4[i++] = encoded_string[in_];
        in_++;
        if (i == 4)
        {
            for (i = 0; i <4; i++)
            {
                char_array_4[i] = base64_chars.find(char_array_4[i]);
            }

            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

            for (i = 0; (i < 3); i++)
            {
                data.resize(data.size() + 1);
                data[data.size() - 1] = char_array_3[i];
            }
            i = 0;
        }
    }

    if (i && encoded_string[in_] == '=')
    {
        for (j = i; j <4; j++)
        {
            char_array_4[j] = 0;
        }

        for (j = 0; j <4; j++)
        {
            char_array_4[j] = base64_chars.find(char_array_4[j]);
        }

        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

        for (j = 0; (j < i - 1); j++)
        {
            data.resize(data.size() + 1);
            data[data.size() - 1] = char_array_3[j];
        }
    }
    return i;
}

PixelData::PixelData(size_t startChannel, const std::string& data, APPLYMETHOD blendMode)
{
    _data = nullptr;
    _startChannel = startChannel;
    _blendMode = blendMode;

    ExtractData(data);
}

PixelData::~PixelData()
{
    if (_data != nullptr)
    {
        free(_data);
    }
}

void PixelData::Set(wxByte* buffer, size_t size)
{
    if (_data != nullptr)
    {
        Blend(buffer, size, _data, _size, _blendMode, _startChannel - 1);
    }
}

void PixelData::ExtractData(const std::string& data)
{
    std::vector<unsigned char> dout;
    base64_decode(data, dout);
    _size = dout.size();

    if (_data != nullptr)
    {
        delete _data;
    }
    _data = (wxByte*)malloc(_size);
    if (_data != nullptr)
    {
        for (size_t i = 0; i < _size; ++i)
        {
            *(_data + i) = dout[i];
        }
    }
}

void PixelData::SetData(const std::string& data, APPLYMETHOD blendMode)
{
    _blendMode = blendMode;

    ExtractData(data);
}

bool ScheduleManager::DoText(PlayListItemText* pliText, const std::string& text, const std::string& properties)
{
    bool valid = true;

    if (pliText == nullptr) return false;

    pliText->SetText(text);

    wxArrayString p = wxSplit(properties, '|');

    for (auto it = p.begin(); it != p.end(); ++it)
    {
        wxArrayString pv = wxSplit(*it, '=');

        if (pv.size() == 2)
        {
            std::string pvl = pv[0].Lower().ToStdString();

            if (pvl == "color" || pvl == "colour")
            {
                wxColour c(pv[1]);
                pliText->SetColour(c);
            }
            else if (pvl == "blendmode")
            {
                std::string vl = pv[1].Lower().ToStdString();
                pliText->SetBlendMode(pv[1].ToStdString());
            }
            else if (pvl == "speed")
            {
                pliText->SetSpeed(wxAtoi(pv[1]));
            }
            else if (pvl == "orientation")
            {
                pliText->SetOrientation(pv[1].ToStdString());
            }
            else if (pvl == "movement")
            {
                pliText->SetOrientation(pv[1].ToStdString());
            }
            else if (pvl == "font")
            {
                wxFont font;
                font.SetNativeFontInfoUserDesc(pv[1]);
                auto f = wxTheFontList->FindOrCreateFont(font.GetPointSize(), font.GetFamily(), font.GetStyle(), font.GetWeight(), font.GetUnderlined(), font.GetFaceName(), font.GetEncoding());
                pliText->SetFont(f);
            }
            else if (pvl == "x")
            {
                pliText->SetX(wxAtoi(pv[1]));
            }
            else if (pvl== "y")
            {
                pliText->SetY(wxAtoi(pv[1]));
            }
            else
            {
                valid = false;
            }
        }
        else
        {
            valid = false;
        }
    }

    return valid;
}

void ScheduleManager::StartVirtualMatrices()
{
    auto v = GetOptions()->GetVirtualMatrices();

    for (auto it = v->begin(); it != v->end(); ++it)
    {
        (*it)->Start();
    }
}

void ScheduleManager::StopVirtualMatrices()
{
    auto v = GetOptions()->GetVirtualMatrices();

    for (auto it = v->begin(); it != v->end(); ++it)
    {
        (*it)->Stop();
    }
}

void ScheduleManager::SendFPPSync(const std::string& syncItem, size_t msec, size_t frameMS)
{
    static std::string lastfseq = "";
    static std::string lastmedia = "";
    static size_t lastfseqmsec = 0;
    static size_t lastmediamsec = 0;

    if (syncItem == "")
    {
        if (lastfseq != "")
        {
            SendFPPSync(lastfseq, 0xFFFFFFFF, 50);
        }

        if (lastmedia != "")
        {
            SendFPPSync(lastmedia, 0xFFFFFFFF, 50);
        }

        return;
    }

    if (_mode == SYNCMODE::FPPMASTER && _fppSyncMaster == nullptr)
    {
        OpenFPPSyncSendSocket();
    }

    if (_fppSyncMaster == nullptr) return;

    bool dosend = false;
    if (msec == 0 || msec == 0xFFFFFFFF) dosend = true;

    wxFileName fn(syncItem.c_str());
    if (fn.GetExt().Lower() == "fseq")
    {
        if (lastfseq != syncItem)
        {
            if (lastfseq != "")
            {
                SendFPPSync(lastfseq, 0xFFFFFFFF, frameMS);
            }

            lastfseq = syncItem;

            if (msec != 0)
            {
                SendFPPSync(syncItem, 0, frameMS);
            }
        }

        if (!dosend)
        {
            if (msec - lastfseqmsec > 1000)
            {
                dosend = true;
            }
        }
    }
    else
    {
        if (lastmedia != syncItem)
        {
            if (lastmedia != "")
            {
                SendFPPSync(lastmedia, 0xFFFFFFFF, frameMS);
            }

            lastmedia = syncItem;

            if (msec != 0)
            {
                SendFPPSync(syncItem, 0, frameMS);
            }
        }

        if (!dosend)
        {
            if (msec - lastmediamsec > 1000 && msec - lastfseqmsec > 500)
            {
                dosend = true;
            }
        }
    }

    if (!dosend) return;

    wxIPV4address remoteAddr;
    //remoteAddr.BroadcastAddress();
    remoteAddr.Hostname("255.255.255.255");
    remoteAddr.Service(FPP_CTRL_PORT);

    wxASSERT(sizeof(ControlPkt) == 7); // ensure data is packed correctly

    int bufsize = sizeof(ControlPkt) + sizeof(SyncPkt) + fn.GetFullName().Length();
    unsigned char* buffer = (unsigned char*)malloc(bufsize);
    memset(buffer, 0x00, bufsize);

    if (buffer != nullptr)
    {
        ControlPkt* cp = (ControlPkt*)buffer;
        strcpy(cp->fppd, "FPPD");
        cp->pktType = CTRL_PKT_SYNC;
        cp->extraDataLen = bufsize - sizeof(ControlPkt);

        SyncPkt* sp = (SyncPkt*)(buffer + sizeof(ControlPkt));

        if (msec == 0)
        {
            sp->pktType = SYNC_PKT_START;
        }
        else if(msec == 0xFFFFFFFF)
        {
            sp->pktType = SYNC_PKT_STOP;
        }
        else
        {
            sp->pktType = SYNC_PKT_SYNC;
        }

        if (fn.GetExt().Lower() == "fseq")
        {
            lastfseqmsec = msec;
            sp->fileType = SYNC_FILE_SEQ;
            sp->frameNumber = msec / frameMS;

            if (msec == 0xFFFFFFFF)
            {
                lastfseq = "";
                lastfseqmsec = 0;
            }
        }
        else
        {
            lastmediamsec = msec;
            sp->fileType = SYNC_FILE_MEDIA;
            sp->frameNumber = 0;

            if (msec == 0xFFFFFFFF)
            {
                lastmedia = "";
                lastmediamsec = 0;
            }
        }

        if (sp->pktType == SYNC_PKT_SYNC)
        {
            sp->secondsElapsed = msec / 1000.0;
        }
        else
        {
            sp->frameNumber = 0;
            sp->secondsElapsed = 0;
        }

        strcpy(&sp->filename[0], fn.GetFullName().c_str());

        _fppSyncMaster->SendTo(remoteAddr, buffer, bufsize);

        free(buffer);
    }
}

void ScheduleManager::OpenFPPSyncSendSocket()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    CloseFPPSyncSendSocket();

    wxIPV4address localaddr;
    if (IPOutput::GetLocalIP() == "")
    {
        localaddr.AnyAddress();
    }
    else
    {
        localaddr.Hostname(IPOutput::GetLocalIP());
    }

    _fppSyncMaster = new wxDatagramSocket(localaddr, wxSOCKET_NOWAIT | wxSOCKET_BROADCAST);
    if (_fppSyncMaster == nullptr)
    {
        logger_base.error("Error opening datagram for FPP Sync as master.");
    }
    else
    {
        logger_base.error("FPP Sync as master datagram opened successfully.");
    }
}

void ScheduleManager::CloseFPPSyncSendSocket()
{
    if (_fppSyncMaster != nullptr) {
        static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
        logger_base.error("FPP Sync as master datagram closed.");
        _fppSyncMaster->Close();
        delete _fppSyncMaster;
        _fppSyncMaster = nullptr;
    }
}

void ScheduleManager::CloseFPPSyncListenSocket()
{
    if (_fppSyncSlave != nullptr) {
        static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
        logger_base.error("FPP Sync as remote datagram closed.");
        _fppSyncSlave->Close();
        delete _fppSyncSlave;
        _fppSyncSlave = nullptr;
    }
}

#define SERVER_ID	200

BEGIN_EVENT_TABLE(ScheduleManager, wxEvtHandler)
EVT_SOCKET(SERVER_ID, ScheduleManager::OnServerEvent)
END_EVENT_TABLE()

void ScheduleManager::OpenFPPSyncListenSocket()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    CloseFPPSyncListenSocket();

    wxIPV4address localaddr;
    if (IPOutput::GetLocalIP() == "")
    {
        localaddr.AnyAddress();
    }
    else
    {
        localaddr.Hostname(IPOutput::GetLocalIP());
    }
    localaddr.Service(FPP_CTRL_PORT);

    _fppSyncSlave = new wxDatagramSocket(localaddr, wxSOCKET_NOWAIT | wxSOCKET_BROADCAST);
    if (_fppSyncSlave == nullptr)
    {
        logger_base.error("Error opening datagram for FPP Sync as remote.");
    }
    else
    {
        _fppSyncSlave->SetEventHandler(*this, SERVER_ID);
        _fppSyncSlave->SetNotify(wxSOCKET_INPUT_FLAG);
        _fppSyncSlave->Notify(true);
        logger_base.error("FPP Sync as remote datagram opened successfully.");
    }
}

void ScheduleManager::StartFSEQ(const std::string fseq)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    // find this fseq file and run it
    PlayList* pl = GetRunningPlayList();
    PlayListStep* pls = nullptr;
    if (pl != nullptr)
    {
        pls = pl->GetStepWithFSEQ(fseq);
        StopPlayList(pl, false);
    }

    if (pls == nullptr)
    {
        for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
        {
            pls = (*it)->GetStepWithFSEQ(fseq);

            if (pls != nullptr) {
                pl = *it;
                break;
            }
        }
    }

    if (pls != nullptr)
    {
        logger_base.debug("... Starting %s %s.", (const char *)pl->GetNameNoTime().c_str(), (const char *)pls->GetNameNoTime().c_str());

        size_t rate;
        PlayPlayList(pl, rate, false, pls->GetNameNoTime(), true);

        wxCommandEvent event1(EVT_FRAMEMS);
        event1.SetInt(rate);
        wxPostEvent(wxGetApp().GetTopWindow(), event1);

        wxCommandEvent event2(EVT_SCHEDULECHANGED);
        wxPostEvent(wxGetApp().GetTopWindow(), event2);
    }
}

void ScheduleManager::OnServerEvent(wxSocketEvent& event)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));

    unsigned char buffer[2048];

    int lastcount = -1;

    while (lastcount != 0)
    {
        wxIPV4address remoteaddr;
        _fppSyncSlave->RecvFrom(remoteaddr, &buffer[0], sizeof(buffer));
        lastcount = _fppSyncSlave->LastCount();

        if (lastcount >= sizeof(ControlPkt) + sizeof(SyncPkt))
        {
            ControlPkt* cp = (ControlPkt*)&buffer[0];
            if (strncmp(cp->fppd, "FPPD", 4) == 0)
            {
                if (cp->pktType == CTRL_PKT_SYNC)
                {
                    SyncPkt* sp = (SyncPkt*)(&buffer[0] + sizeof(ControlPkt));

                    if (sp->fileType == SYNC_FILE_SEQ)
                    {
                        switch (sp->pktType)
                        {
                        case SYNC_PKT_START:
                            {
                                logger_base.debug("Remote start %s.", (const char *)sp->filename);

                                StartFSEQ(std::string(sp->filename));
                            }
                            break;
                        case SYNC_PKT_STOP:
                            {
                                logger_base.debug("Remote stop %s.", (const char *)sp->filename);
                                PlayList* pl = GetRunningPlayList();
                                if (pl != nullptr)
                                {
                                    PlayListStep* pls = pl->GetRunningStep();

                                    if (pls != nullptr && pls->IsRunningFSEQ(sp->filename))
                                    {
                                        logger_base.debug("... Stopping %s %s.", (const char *)pl->GetNameNoTime().c_str(), (const char *)pls->GetNameNoTime().c_str());

                                        // if this fseq file is running stop it
                                        StopPlayList(pl, false);
                                        wxCommandEvent event2(EVT_SCHEDULECHANGED);
                                        wxPostEvent(wxGetApp().GetTopWindow(), event2);
                                    }
                                }
                            }
                            break;
                        case SYNC_PKT_SYNC:
                            {
                                PlayList* pl = GetRunningPlayList();

                                if (pl == nullptr)
                                {
                                    StartFSEQ(std::string(sp->filename));
                                    pl = GetRunningPlayList();
                                }

                                if (pl != nullptr)
                                {
                                    PlayListStep* pls = pl->GetRunningStep();

                                    if (pls != nullptr && pls->IsRunningFSEQ(sp->filename))
                                    {
                                        // if this fseq file is running stop it
                                        pls->SetSyncPosition(sp->frameNumber, (size_t)sp->secondsElapsed * 1000);
                                    }
                                }
                            }
                            break;
                        }
                    }
                    else
                    {
                        // media file ... not sure what to do with this ... so ignoring it
                    }
                }
            }
        }
    }
}

PlayListItem* ScheduleManager::FindRunProcessNamed(const std::string& item) const
{
    PlayListItem *pli = nullptr;

    for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
    {
        pli = (*it)->FindRunProcessNamed(item);

        if (pli != nullptr) break;
    }

    return pli;
}

PlayListStep* ScheduleManager::GetStepContainingPlayListItem(wxUint32 id) const
{
    PlayListStep *pls = nullptr;

    for (auto it = _playLists.begin(); it != _playLists.end(); ++it)
    {
        pls = (*it)->GetStepContainingPlayListItem(id);

        if (pls != nullptr) break;
    }

    return pls;
}
