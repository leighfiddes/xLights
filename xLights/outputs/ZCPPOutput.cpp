#include "ZCPPOutput.h"

#include <wx/xml/xml.h>
#include <wx/file.h>
#include <wx/filename.h>

#include "ZCPPDialog.h"
#include "OutputManager.h"
#include "../UtilFunctions.h"

#include <list>

#include <log4cpp/Category.hh>

#pragma region Constructors and Destructors
ZCPPOutput::ZCPPOutput(wxXmlNode* node, std::string showdir) : IPOutput(node)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    memset(_modelData, 0x00, sizeof(_modelData));
    _lastSecond = -1;
    _sequenceNum = 0;
    _usedChannels = _channels;
    _datagram = nullptr;
    _data = (wxByte*)malloc(_channels);
    memset(_data, 0, _channels);
    memset(_packet, 0, sizeof(_packet));
    _vendor = wxAtoi(node->GetAttribute("Vendor", "65535"));
    _model = wxAtoi(node->GetAttribute("Model", "65535"));
    _sendConfiguration = node->GetAttribute("SendConfig", "TRUE") != "FALSE";

    wxString fileName = GetIP();
    fileName.Replace(".", "_");
    fileName += ".zcpp";
    fileName = showdir + wxFileName::GetPathSeparator() + fileName;

    if (wxFile::Exists(fileName))
    {
        wxFile zf;
        if (zf.Open(fileName))
        {
            zf.Read(_modelData, sizeof(_modelData));
            while (!zf.Eof())
            {
                wxByte* descPacket = (wxByte*)malloc(ZCPP_EXTRACONFIG_PACKET_SIZE);
                zf.Read(descPacket, ZCPP_EXTRACONFIG_PACKET_SIZE);
                _extraConfig.push_back(descPacket);
            }
            zf.Close();
            logger_base.debug("ZCPP Model data file %s loaded.", (const char*)fileName.c_str());
            ExtractUsedChannelsFromModelData();
        }
        else
        {
            logger_base.warn("ZCPP Model data file %s could not be opened.", (const char*)fileName.c_str());
        }
    }
    else
    {
        logger_base.warn("ZCPP Model data file %s not found.", (const char*)fileName.c_str());
    }
}

ZCPPOutput::ZCPPOutput() : IPOutput()
{
    memset(_modelData, 0x00, sizeof(_modelData));
    _lastSecond = -1;
    _channels = 1;
    _usedChannels = 1;
    _universe = -1;
    _sequenceNum = 0;
    _sendConfiguration = true;
    _datagram = nullptr;
    _vendor = -1;
    _autoSize = true;
    _model = -1;
    _data = (wxByte*)malloc(_channels);
    memset(_data, 0, _channels);
    memset(_packet, 0, sizeof(_packet));
}

ZCPPOutput::~ZCPPOutput()
{
    if (_datagram != nullptr) delete _datagram;
    if (_data != nullptr) free(_data);
    while (_extraConfig.size() > 0)
    {
        free(_extraConfig.front());
        _extraConfig.pop_front();
    }
}
#pragma endregion Constructors and Destructors

void ZCPPOutput::ExtractUsedChannelsFromModelData()
{
    int ports = _modelData[38];

    _usedChannels = 1;
    for (int i = 0; i < ports; i++)
    {
        int start = (((int)_modelData[41 + i * 10]) << 24) +
            (((int)_modelData[42 + i * 10]) << 16) +
            (((int)_modelData[43 + i * 10]) << 8) +
            (((int)_modelData[44 + i * 10]));
        int len = (((int)_modelData[45 + i * 10]) << 8) +
            (((int)_modelData[46 + i * 10]));
        if (start + len - 1 > _usedChannels)
        {
            _usedChannels = start + len;
        }
    }
}

bool ZCPPOutput::SetModelData(unsigned char* buffer, size_t bufsize, std::list<wxByte*> extraConfig, std::string showDir)
{
    // before byte 9 there can be differences
    if (memcmp(&_modelData[8], &buffer[8], std::min(bufsize, sizeof(_modelData)) - 8) == 0 &&
        _extraConfig.size() == _extraConfig.size())
    {
        bool extraConfigSame = true;
        auto it1 = _extraConfig.begin();
        auto it2 = extraConfig.begin();

        while (extraConfigSame && it1 != _extraConfig.end())
        {
            if (memcmp(&(*it1)[8], &(*it2)[8], ZCPP_EXTRACONFIG_PACKET_SIZE - 8) != 0)
            {
                extraConfigSame = false;
            }
            ++it1;
            ++it2;
        }

        if (extraConfigSame)
        {
            // nothing has changed
            return false;
        }
    }

    while (_extraConfig.size() > 0)
    {
        free(_extraConfig.front());
        _extraConfig.pop_front();
    }

    for (auto it = extraConfig.begin(); it != extraConfig.end(); ++it)
    {
        _extraConfig.push_back(*it);
    }

    wxString fileName = GetIP();
    fileName.Replace(".", "_");
    fileName += ".zcpp";
    fileName = showDir + wxFileName::GetPathSeparator() + fileName;

    wxFile zf;
    if (zf.Create(fileName, true))
    {
        zf.Write(buffer, bufsize);
        for (auto it = _extraConfig.begin(); it != _extraConfig.end(); ++it)
        {
            zf.Write(*it, ZCPP_EXTRACONFIG_PACKET_SIZE);
        }
        zf.Close();
    }

    wxASSERT(bufsize <= ZCPP_MODELDATASIZE);
    memcpy(_modelData, buffer, std::min(bufsize, sizeof(_modelData)));
    _lastSecond = -1;

    ExtractUsedChannelsFromModelData();

    return true;
}

wxXmlNode* ZCPPOutput::Save()
{
    wxXmlNode* node = new wxXmlNode(wxXML_ELEMENT_NODE, "network");
    node->AddAttribute("Vendor", wxString::Format("%d", _vendor));
    node->AddAttribute("Model", wxString::Format("%d", _model));
    if (!_sendConfiguration) node->AddAttribute("SendConfiguration", "FALSE");
    IPOutput::Save(node);

    return node;
}

#pragma region Static Functions
void ZCPPOutput::SendSync(int syncUniverse)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    static wxByte syncdata[ZCPP_SYNCPACKET_LEN];
    static wxByte syncSequenceNum = 0;
    static bool initialised = false;
    static wxIPV4address syncremoteAddr;
    static wxDatagramSocket *syncdatagram = nullptr;

    if (!initialised)
    {
        logger_base.debug("Initialising ZCPP Sync.");

        initialised = true;

        memset(syncdata, 0x00, sizeof(syncdata));

        syncdata[0] = 'Z';   // RLP preamble size (low)
        syncdata[1] = 'C';   // ACN Packet Identifier (12 bytes)
        syncdata[2] = 'P';
        syncdata[3] = 'P';
        syncdata[4] = 21;
        syncdata[5] = 0x00;

        wxIPV4address localaddr;
        if (IPOutput::__localIP == "")
        {
            localaddr.AnyAddress();
        }
        else
        {
            localaddr.Hostname(IPOutput::__localIP);
        }

        if (syncdatagram != nullptr)
        {
            delete syncdatagram;
        }

        syncdatagram = new wxDatagramSocket(localaddr, wxSOCKET_NOWAIT);

        if (syncdatagram == nullptr)
        {
            logger_base.error("Error initialising ZCPP sync datagram.");
        }
        else if (!syncdatagram->IsOk())
        {
            logger_base.error("Error initialising ZCPP sync datagram ... is network connected? OK : FALSE");
            delete syncdatagram;
            syncdatagram = nullptr;
        }
        else if (syncdatagram->Error() != wxSOCKET_NOERROR)
        {
            logger_base.error("Error creating ZCPP sync datagram => %d : %s.", syncdatagram->LastError(), (const char *)DecodeIPError(syncdatagram->LastError()).c_str());
            delete syncdatagram;
            syncdatagram = nullptr;
        }

        // multicast - universe number must be in lower 2 bytes
        wxString ipaddrWithUniv = ZCPP_MULTICAST_TO;
        syncremoteAddr.Hostname(ipaddrWithUniv);
        syncremoteAddr.Service(ZCPP_PORT);
    }

    syncdata[6] = syncSequenceNum++;   // sequence number

    // bail if we dont have a datagram to use
    if (syncdatagram != nullptr)
    {
        syncdatagram->SendTo(syncremoteAddr, syncdata, ZCPP_SYNCPACKET_LEN);
    }
}

void ZCPPOutput::InitialiseExtraConfigPacket(wxByte* buffer, int seq, std::string userControllerId)
{
    memset(buffer, 0x00, ZCPP_EXTRACONFIG_PACKET_SIZE);
    buffer[0] = 'Z';
    buffer[1] = 'C';   // ACN Packet Identifier (12 bytes)
    buffer[2] = 'P';
    buffer[3] = 'P';
    buffer[4] = 0x0B;
    buffer[5] = 0x00;
    buffer[6] = (seq & 0xff00) >> 8;
    buffer[7] = seq & 0xff;
    strncpy((char*)&buffer[8], userControllerId.c_str(), 30);
}

std::list<Output*> ZCPPOutput::Discover(OutputManager* outputManager)
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    std::list<Output*> res;

    wxByte packet[7];

    packet[0] = 'Z';
    packet[1] = 'C';
    packet[2] = 'P';
    packet[3] = 'P';
    packet[4] = 0x00;
    packet[5] = 0x00;
    packet[6] = 0x00;

    wxIPV4address sendlocaladdr;
    if (IPOutput::__localIP == "")
    {
        sendlocaladdr.AnyAddress();
    }
    else
    {
        sendlocaladdr.Hostname(IPOutput::__localIP);
    }

    wxDatagramSocket* datagram = new wxDatagramSocket(sendlocaladdr, wxSOCKET_NOWAIT | wxSOCKET_BROADCAST);

    if (datagram == nullptr)
    {
        logger_base.error("Error initialising ZCPP discovery datagram.");
    }
    else if (!datagram->IsOk())
    {
        logger_base.error("Error initialising ZCPP discovery datagram ... is network connected? OK : FALSE");
        delete datagram;
        datagram = nullptr;
    }
    else if (datagram->Error() != wxSOCKET_NOERROR)
    {
        logger_base.error("Error creating ZCPP discovery datagram => %d : %s.", datagram->LastError(), (const char *)DecodeIPError(datagram->LastError()).c_str());
        delete datagram;
        datagram = nullptr;
    }
    else
    {
        logger_base.info("ZCPP discovery datagram opened successfully.");
    }

    // multicast - universe number must be in lower 2 bytes
    wxIPV4address remoteaddr;
    wxString ipaddrWithUniv = ZCPP_MULTICAST_TO;
    remoteaddr.Hostname(ipaddrWithUniv);
    remoteaddr.Service(ZCPP_PORT);

    // bail if we dont have a datagram to use
    if (datagram != nullptr)
    {
        logger_base.info("ZCPP sending discovery packet.");
        datagram->SendTo(remoteaddr, packet, sizeof(packet));
        if (datagram->Error() != wxSOCKET_NOERROR)
        {
            logger_base.error("Error sending ZCPP discovery datagram => %d : %s.", datagram->LastError(), (const char *)DecodeIPError(datagram->LastError()).c_str());
        }
        else
        {
            logger_base.info("ZCPP sent discovery packet. Sleeping for 2 seconds.");

            // give the controllers 2 seconds to respond
            wxMilliSleep(2000);

            unsigned char buffer[2048];

            int lastread = 1;

            while (lastread > 0)
            {
                wxStopWatch sw;
                logger_base.debug("Trying to read ZCPP discovery packet.");
                memset(buffer, 0x00, sizeof(buffer));
                datagram->Read(&buffer[0], sizeof(buffer));
                lastread = datagram->LastReadCount();

                if (lastread > 0)
                {
                    logger_base.debug(" Read done. %d bytes %ldms", lastread, sw.Time());

                    if (buffer[0] == 'Z' && buffer[1] == 'C' && buffer[2] == 'P' && buffer[3] == 'P' && buffer[4] == 0x01)
                    {
                        logger_base.debug(" Valid response.");

                        long channels = ((long)buffer[66] << 24) + ((long)buffer[67] << 16) + ((long)buffer[68] << 8) + (long)buffer[69];
                        ZCPPOutput* output = new ZCPPOutput();
                        output->SetDescription(std::string((char*)&buffer[31]));
                        auto ip = wxString::Format("%d.%d.%d.%d", (int)buffer[110], (int)buffer[111], (int)buffer[112], (int)buffer[113]);
                        output->SetIP(ip.ToStdString());
                        int vendor = ((int)buffer[6] << 8) + buffer[7];
                        output->SetVendor(vendor);
                        int model = ((int)buffer[8] << 8) + buffer[9];
                        output->SetModel(model);

                        // now search for it in outputManager
                        auto outputs = outputManager->GetOutputs();
                        for (auto it = outputs.begin(); it != outputs.end(); ++it)
                        {
                            if ((*it)->GetIP() == output->GetIP())
                            {
                                // we already know about this controller
                                logger_base.info("ZCPP Discovery we already know about this controller %s.", (const char*)output->GetIP().c_str());
                                delete output;
                                output = nullptr;
                                break;
                            }
                        }

                        if (buffer[115] & 0x04)
                        {
                            // Dan this is where you would need to do your special adjustments to ensure it is in the right place
                            logger_base.info("ZCPP Discovery found controller %s but it doesnt want us to configure it.", (const char*)output->GetIP().c_str());
                            delete output;
                            output = nullptr;
                            break;
                        }
                        else
                        {
                            output->SetAutoSize(true);
                            output->SetChannels(1 /*channels*/); // Set this to one as it defaults to auto size
                            output->SetSendConfiguration(true);
                        }

                        if (output != nullptr)
                        {
                            logger_base.info("ZCPP Discovery adding controller %s.", (const char*)output->GetIP().c_str());
                            res.push_back(output);
                        }
                    }
                    else
                    {
                        // non discovery response packet
                        logger_base.info("ZCPP Discovery strange packet received.");
                    }
                }
            }
            logger_base.info("ZCPP Discovery Done looking for response.");
        }
        datagram->Close();
        delete datagram;
    }

    logger_base.info("ZCPP Discovery Finished.");

    return res;
}
#pragma endregion Static Functions

#pragma region Start and Stop
bool ZCPPOutput::Open()
{
    static log4cpp::Category &logger_base = log4cpp::Category::getInstance(std::string("log_base"));
    if (!_enabled) return true;

    _lastSecond = -1;

    _ok = IPOutput::Open();

    memset(_packet, 0x00, sizeof(_packet));
    _sequenceNum = 0;

    _packet[0] = 'Z';
    _packet[1] = 'C';
    _packet[2] = 'P';
    _packet[3] = 'P';
    _packet[4] = 20;
    _packet[5] = 0x00;

    wxIPV4address localaddr;
    if (IPOutput::__localIP == "")
    {
        localaddr.AnyAddress();
    }
    else
    {
        localaddr.Hostname(IPOutput::__localIP);
    }

    _datagram = new wxDatagramSocket(localaddr, wxSOCKET_NOWAIT);
    if (_datagram == nullptr)
    {
        logger_base.error("ZCPPOutput: Error opening datagram.");
    }
    else if (!_datagram->IsOk())
    {
        logger_base.error("ZCPPOutput: Error opening datagram. Network may not be connected? OK : FALSE");
        delete _datagram;
        _datagram = nullptr;
    }
    else if (_datagram->Error() != wxSOCKET_NOERROR)
    {
        logger_base.error("Error creating ZCPP datagram => %d : %s.", _datagram->LastError(), (const char *)DecodeIPError(_datagram->LastError()).c_str());
        delete _datagram;
        _datagram = nullptr;
    }

    _remoteAddr.Hostname(_ip.c_str());
    _remoteAddr.Service(ZCPP_PORT);

    return _ok && _datagram != nullptr;
}

void ZCPPOutput::Close()
{
    if (_datagram != nullptr)
    {
        delete _datagram;
        _datagram = nullptr;
    }
}
#pragma endregion Start and Stop

void ZCPPOutput::SetTransientData(int on, long startChannel, int nullnumber)
{
    _outputNumber = on;
    _startChannel = startChannel;
    if (nullnumber > 0) _nullNumber = nullnumber;
}

#pragma region Frame Handling
void ZCPPOutput::StartFrame(long msec)
{
    if (!_enabled) return;

    _timer_msec = msec;
}

void ZCPPOutput::EndFrame(int suppressFrames)
{
    if (!_enabled || _suspend) return;

    if (_datagram == nullptr || _usedChannels == 0) return;

    if (IsSendConfiguration())
    {
        long second = wxGetLocalTime();
        if (_lastSecond == -1 || (second - _lastSecond) % 10 == 0)
        {
            if (_lastSecond == -1 || second % 600 == 0)
            {
                // Send descriptions every 10 mins
                for (auto it = _extraConfig.begin(); it != _extraConfig.end(); ++it)
                {
                    _datagram->SendTo(_remoteAddr, *it, ZCPP_EXTRACONFIG_PACKET_SIZE);
                }
            }

            if (_modelData[0] != 0x00)
            {
                _lastSecond = second;
                _datagram->SendTo(_remoteAddr, _modelData, sizeof(_modelData));
            }
        }
    }

    if (_changed || NeedToOutput(suppressFrames))
    {
        int i = 0;
        while (i < _usedChannels)
        {
            _packet[6] = _sequenceNum;
            long startAddress = i;
            _packet[7] = (wxByte)((startAddress >> 24) & 0xFF);
            _packet[8] = (wxByte)((startAddress >> 16) & 0xFF);
            _packet[9] = (wxByte)((startAddress >> 8) & 0xFF);
            _packet[10] = (wxByte)((startAddress) & 0xFF);
            int packetlen = _usedChannels - i > ZCPP_PACKET_LEN - 14 ? ZCPP_PACKET_LEN - 14 : _usedChannels - i;
            _packet[11] = (OutputManager::IsSyncEnabled_() ? 0x01 : 0x00) + (i + packetlen == _usedChannels ? 0x80 : 0x00);
            _packet[12] = (wxByte)((packetlen >> 8) & 0xFF);
            _packet[13] = (wxByte)((packetlen) & 0xFF);
            memcpy(&_packet[14], &_data[i], packetlen);
            _datagram->SendTo(_remoteAddr, _packet, 14 + packetlen);
            i += packetlen;
        }
        _sequenceNum = _sequenceNum == 255 ? 0 : _sequenceNum + 1;

        FrameOutput();
    }
    else
    {
        SkipFrame();
    }
}

void ZCPPOutput::ResetFrame()
{
    if (!_enabled) return;
}
#pragma endregion Frame Handling

#pragma region Data Setting
void ZCPPOutput::SetOneChannel(long channel, unsigned char data)
{
    if (!_enabled) return;

    if (_data[channel] != data) {
        _data[channel] = data;
        _changed = true;
    }
}

void ZCPPOutput::SetManyChannels(long channel, unsigned char data[], long size)
{
        long chs = std::min(size, _channels - channel);

        if (memcmp(&_data[channel], data, chs) == 0)
        {
            // nothing changed
        }
        else
        {
            memcpy(&_data[channel], data, chs);
            _changed = true;
        }
}

void ZCPPOutput::AllOff()
{
    memset(_data, 0x00, _channels);
    _changed = true;
}
#pragma endregion Data Setting

#pragma region Getters and Setters
long ZCPPOutput::GetEndChannel() const
{
    return _startChannel + _channels - 1;
}

std::string ZCPPOutput::GetLongDescription() const
{
    std::string res = "";

        if (!_enabled) res += "INACTIVE ";
        res += "ZCPP " + _ip;
        res += " [1-" + std::string(wxString::Format(wxT("%li"), (long)_channels)) + "] ";
        res += "(" + std::string(wxString::Format(wxT("%li"), (long)GetStartChannel())) + "-" + std::string(wxString::Format(wxT("%li"), (long)GetActualEndChannel())) + ") ";
        res += _description;

    return res;
}

std::string ZCPPOutput::GetChannelMapping(long ch) const
{
    std::string res = "Channel " + std::string(wxString::Format(wxT("%li"), ch)) + " maps to ...\n";

    res += "Type: ZCPP\n";
    long channeloffset = ch - GetStartChannel() + 1;
    res += "IP: " + _ip + "\n";
    res += "Channel: " + std::string(wxString::Format(wxT("%li"), channeloffset)) + "\n";

    if (!_enabled) res += " INACTIVE";
    return res;
}
#pragma endregion Getters and Setters

#pragma region UI
#ifndef EXCLUDENETWORKUI
Output* ZCPPOutput::Configure(wxWindow* parent, OutputManager* outputManager)
{
    ZCPPDialog dlg(parent, this, outputManager);

    int res = dlg.ShowModal();

    if (res == wxID_CANCEL)
    {
        return nullptr;
    }

    return this;
}
#endif
#pragma endregion UI