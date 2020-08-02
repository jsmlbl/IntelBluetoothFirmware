/* add your code here */
#include "BtIntel.h"

#include "IntelBluetoothFirmware.hpp"
#include <libkern/libkern.h>
#include <libkern/OSKextLib.h>
#include <libkern/version.h>
#include <libkern/OSTypes.h>
#include <IOKit/usb/StandardUSB.h>
#include "Hci.h"

#define super IOService
OSDefineMetaClassAndStructors(IntelBluetoothFirmware, IOService)

#define kReadBufferSize 4096
//com.apple.iokit.IOBluetoothHostControllerUSBTransport

enum { kMyOffPowerState = 0, kMyOnPowerState = 1 };

enum {
    kReset,
    kGetIntelVersion,
    kEnterMfg,
    kLoadFW,
    kExitMfg,
    kSetEventMask,
    kUpdateAbort,
    kUpdateDone
};

enum {
    kNewGetVersion,
    kNewGetBootParams,
    kNewLoadFW,
    kNewIntelReset,
    kNewSetEventMask,
    kNewResetToBL,
    kNewUpdateAbort,
    kNewUpdateDone
};

#define kIOPMPowerOff 0

static IOPMPowerState myTwoStates[2] =
{
    {1, kIOPMPowerOff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

bool IntelBluetoothFirmware::init(OSDictionary *dictionary)
{
    XYLog("Driver init()\n");
    hciCommand = (HciCommandHdr *)IOMalloc(sizeof(HciCommandHdr));
    
    completion = IOLockAlloc();
    
    if (!completion) {
        return false;
    }
    return super::init(dictionary);
}

void IntelBluetoothFirmware::free() {
    XYLog("Driver free()\n");
    if (hciCommand) {
        IOFree(hciCommand, sizeof(HciCommandHdr));
        hciCommand = NULL;
    }
    if (completion) {
        IOLockFree(completion);
        completion = NULL;
    }
    super::free();
}

bool IntelBluetoothFirmware::start(IOService *provider)
{
    XYLog("Driver Start()\n");
    m_pDevice = OSDynamicCast(IOUSBHostDevice, provider);
    
    if (m_pDevice == NULL) {
        XYLog("Driver Start fail, not usb device\n");
        return false;
    }
    
    PMinit();
    registerPowerDriver(this, myTwoStates, 2);
    provider->joinPMtree(this);
    makeUsable();
    
    mReadBuffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task
                                                              , kIODirectionIn, kReadBufferSize);
    if (!mReadBuffer) {
        XYLog("%s::fail to alloc read buffer\n", getName());
        cleanUp();
        stop(this);
        return false;
    }
    
    mReadBuffer->prepare(kIODirectionIn);
    
    super::start(provider);
    
    m_pDevice->setConfiguration(0);
    
    IOSleep(1500);
    
    usbCompletion.owner = this;
    usbCompletion.action = onRead;
    usbCompletion.parameter = NULL;
    
    if (!m_pDevice->open(this)) {
        XYLog("start fail, can not open device\n");
        cleanUp();
        stop(this);
        return false;
    }
    
    if (!initUSBConfiguration()) {
        XYLog("init usb configuration failed\n");
        cleanUp();
        stop(this);
        return false;
    }
    if (!initInterface()) {
        XYLog("init usb interface failed\n");
        cleanUp();
        stop(this);
        return false;
    }
    XYLog("usb init succeed\n");
    if (currentType == kTypeOld) {
        beginDownload();
    } else {
        beginDownloadNew();
    }
    cleanUp();
    return true;
}

bool IntelBluetoothFirmware::initUSBConfiguration()
{
    uint8_t configIndex = 0;
    uint8_t configNum = m_pDevice->getDeviceDescriptor()->bNumConfigurations;
    if (configNum < configIndex + configNum) {
        XYLog("config num error\n");
        return false;
    }
    const StandardUSB::ConfigurationDescriptor *configDescriptor = m_pDevice->getConfigurationDescriptor(configIndex);
    if (!configDescriptor) {
        XYLog("getConfigurationDescriptor(%d) failed\n", configIndex);
        return false;
    }
    XYLog("set configuration to %d\n", configDescriptor->bConfigurationValue);
    IOReturn ret = m_pDevice->setConfiguration(configDescriptor->bConfigurationValue);
    if (ret != kIOReturnSuccess) {
        XYLog("set device configuration to %d failed\n", configDescriptor->bConfigurationValue);
        return false;
    }
    return true;
}

bool IntelBluetoothFirmware::initInterface()
{
    OSIterator* iterator = m_pDevice->getChildIterator(gIOServicePlane);
    OSObject* candidate = NULL;
    if (iterator == NULL) {
        XYLog("can not create child iterator\n");
        return false;
    }
    while((candidate = iterator->getNextObject()) != NULL)
    {
        IOUSBHostInterface* interfaceCandidate = OSDynamicCast(IOUSBHostInterface, candidate);
        if(   interfaceCandidate != NULL
           )
        {
            XYLog("Found interface!!!\n");
            m_pInterface = interfaceCandidate;
            break;
        }
    }
    OSSafeReleaseNULL(iterator);
    if (m_pInterface == NULL) {
        return false;
    }
    if (!m_pInterface->open(this)) {
        XYLog("can not open interface\n");
        return false;
    }
    const StandardUSB::ConfigurationDescriptor *configDescriptor = m_pInterface->getConfigurationDescriptor();
    const StandardUSB::InterfaceDescriptor *interfaceDescriptor = m_pInterface->getInterfaceDescriptor();
    if (configDescriptor == NULL || interfaceDescriptor == NULL) {
        XYLog("find descriptor NULL\n");
        return false;
    }
    const EndpointDescriptor *endpointDescriptor = NULL;
    while ((endpointDescriptor = StandardUSB::getNextEndpointDescriptor(configDescriptor, interfaceDescriptor, endpointDescriptor))) {
        uint8_t epDirection = StandardUSB::getEndpointDirection(endpointDescriptor);
        uint8_t epType = StandardUSB::getEndpointType(endpointDescriptor);
        if (epDirection == kUSBIn && epType == kUSBInterrupt) {
            XYLog("Found Interrupt endpoint!\n");
            m_pInterruptReadPipe = m_pInterface->copyPipe(StandardUSB::getEndpointAddress(endpointDescriptor));
            if (m_pInterruptReadPipe == NULL) {
                XYLog("copy InterruptReadPipe pipe fail\n");
                return false;
            }
            m_pInterruptReadPipe->retain();
            m_pInterruptReadPipe->release();
        } else {
            if (epDirection == kUSBOut && epType == kUSBBulk) {
                XYLog("Found Bulk out endpoint!\n");
                m_pBulkWritePipe = m_pInterface->copyPipe(StandardUSB::getEndpointAddress(endpointDescriptor));
                if (m_pBulkWritePipe == NULL) {
                    XYLog("copy Bulk pipe fail\n");
                    return false;
                }
                m_pBulkWritePipe->retain();
                m_pBulkWritePipe->release();
            } else {
                if (epDirection == kUSBIn && epType == kUSBBulk) {
                    XYLog("Found Bulk in endpoint!\n");
                    m_pBulkReadPipe = m_pInterface->copyPipe(StandardUSB::getEndpointAddress(endpointDescriptor));
                    if (m_pBulkReadPipe == NULL) {
                        XYLog("copy Bulk pipe fail\n");
                        return false;
                    }
                    m_pBulkReadPipe->retain();
                    m_pBulkReadPipe->release();
                }
            }
        }
    }
    return m_pInterruptReadPipe != NULL && m_pBulkWritePipe != NULL;
}

void IntelBluetoothFirmware::cleanUp()
{
    XYLog("Clean up...\n");
    if (fwData) {
        fwData->release();
        fwData = NULL;
    }
    if (m_pBulkWritePipe) {
        m_pBulkWritePipe->abort();
        m_pBulkWritePipe->release();
        m_pBulkWritePipe = NULL;
    }
    if (m_pBulkReadPipe) {
        m_pBulkReadPipe->abort();
        m_pBulkReadPipe->release();
        m_pBulkReadPipe = NULL;
    }
    if (m_pInterruptReadPipe) {
        m_pInterruptReadPipe->abort();
        m_pInterruptReadPipe->release();
        m_pInterruptReadPipe = NULL;
    }
    if (mReadBuffer) {
        mReadBuffer->complete(kIODirectionIn);
        usbCompletion.owner = NULL;
        usbCompletion.action = NULL;
        OSSafeReleaseNULL(mReadBuffer);
    }
    if (m_pInterface) {
        m_pInterface->close(this);
        m_pInterface = NULL;
    }
    if (m_pDevice) {
        m_pDevice->setConfiguration(0);
        m_pDevice->close(this);
        m_pDevice = NULL;
    }
}

bool IntelBluetoothFirmware::interruptPipeRead()
{
    IOReturn result;
    isRequest = false;
    if ((result = m_pInterruptReadPipe->io(mReadBuffer, (uint32_t)mReadBuffer->getLength(), &usbCompletion, 0)) != kIOReturnSuccess) {
        if (result == kIOUSBPipeStalled)
        {
            XYLog("%s pipe stall, try clear\n", __FUNCTION__);
            bool clearSucceed = false;
            for (int i = 0; i < 1000; i++) {
                if (m_pInterruptReadPipe->clearStall(true) != kIOUSBPipeStalled) {
                    XYLog("%s %d clear stall succeed.\n", __FUNCTION__, i);
                    clearSucceed = true;
                    break;
                }
            }
            if (clearSucceed) {
                return (result = m_pInterruptReadPipe->io(mReadBuffer, (uint32_t)mReadBuffer->getLength(), &usbCompletion, 0)) != kIOReturnSuccess;
            }
        }
        return false;
    }
    return true;
}

bool IntelBluetoothFirmware::bulkPipeRead()
{
    IOReturn result;
    isRequest = false;
    if ((result = m_pBulkReadPipe->io(mReadBuffer, (uint32_t)mReadBuffer->getLength(), &usbCompletion, 0)) != kIOReturnSuccess) {
        if (result == kIOUSBPipeStalled)
        {
            XYLog("%s pipe stall, try clear\n", __FUNCTION__);
            bool clearSucceed = false;
            for (int i = 0; i < 1000; i++) {
                if (m_pBulkReadPipe->clearStall(true) != kIOUSBPipeStalled) {
                    XYLog("%s %d clear stall succeed.\n", __FUNCTION__, i);
                    clearSucceed = true;
                    break;
                }
            }
            if (clearSucceed) {
                return (result = m_pBulkReadPipe->io(mReadBuffer, (uint32_t)mReadBuffer->getLength(), &usbCompletion, 0)) != kIOReturnSuccess;
            }
        }
        return false;
    }
    return true;
}

void IntelBluetoothFirmware::beginDownload()
{
    mDeviceState = kReset;
    bool isSucceed = false;
    while (true) {
        
        if (mDeviceState == kUpdateDone || mDeviceState == kUpdateAbort) {
            break;
        }
        
        IOReturn ret;
        switch (mDeviceState) {
            case kReset:
            {
                XYLog("HCI_RESET\n");
                if ((ret = sendHCIRequest(HCI_OP_RESET, 0, NULL)) != kIOReturnSuccess) {
                    XYLog("sending initial HCI reset command failed (%d), %s\n ", ret, stringFromReturn(ret));
                    goto done;
                    break;
                }
                break;
            }
            case kGetIntelVersion:
            {
                XYLog("HCI_OP_INTEL_VERSION\n");
                if ((ret = sendHCIRequest(HCI_OP_INTEL_VERSION, 0, NULL)) != kIOReturnSuccess) {
                    XYLog("Reading Intel version information failed, ret=%d %s\n", ret, stringFromReturn(ret));
                    goto done;
                    break;
                }
                break;
            }
            case kEnterMfg:
            {
                XYLog("HCI_OP_INTEL_ENTER_MFG\n");
                if ((ret = sendHCIRequest(HCI_OP_INTEL_ENTER_MFG, 2, (void *)ENTER_MFG_PARAM)) != kIOReturnSuccess) {
                    XYLog("Entering manufacturer mode failed (%d) %s\n", ret, stringFromReturn(ret));
                    goto done;
                    break;
                }
                break;
            }
            case kLoadFW:
            {
                uint8_t* fw = (uint8_t*)fwData->getBytesNoCopy();
                uint8_t* fw_ptr = (uint8_t*)fw;
                while (fwData->getLength() > fw_ptr - fw) {
                    FWCommandHdr *cmd;
                    const uint8_t *cmd_param;
                    HciEventHdr *evt = NULL;
                    const uint8_t *evt_param = NULL;
                    int remain = fwData->getLength() - (fw_ptr - fw);
                    /* The first byte indicates the types of the patch command or event.
                     * 0x01 means HCI command and 0x02 is HCI event. If the first bytes
                     * in the current firmware buffer doesn't start with 0x01 or
                     * the size of remain buffer is smaller than HCI command header,
                     * the firmware file is corrupted and it should stop the patching
                     * process.
                     */
                    if (remain > HCI_COMMAND_HDR_SIZE && fw_ptr[0] != 0x01) {
                        XYLog("Intel fw corrupted: invalid cmd read\n");
                        mDeviceState = kUpdateAbort;
                        goto done;
                        break;
                    }
                    fw_ptr++;
                    remain--;
                    cmd = (FWCommandHdr*)(fw_ptr);
                    fw_ptr += sizeof(*cmd);
                    remain -= sizeof(*cmd);
                    /* Ensure that the remain firmware data is long enough than the length
                     * of command parameter. If not, the firmware file is corrupted.
                     */
                    if (remain < cmd->plen) {
                        XYLog("Intel fw corrupted: invalid cmd len\n");
                        goto done;
                        break;
                    }
                    /* If there is a command that loads a patch in the firmware
                     * file, then enable the patch upon success, otherwise just
                     * disable the manufacturer mode, for example patch activation
                     * is not required when the default firmware patch file is used
                     * because there are no patch data to load.
                     */
                    
                    cmd_param = fw_ptr;
                    fw_ptr += cmd->plen;
                    remain -= cmd->plen;
                    /* This reads the expected events when the above command is sent to the
                     * device. Some vendor commands expects more than one events, for
                     * example command status event followed by vendor specific event.
                     * For this case, it only keeps the last expected event. so the command
                     * can be sent with __hci_cmd_sync_ev() which returns the sk_buff of
                     * last expected event.
                     */
                    int evt_times = 0;
                    while (remain > HCI_EVENT_HDR_SIZE && fw_ptr[0] == 0x02) {
                        fw_ptr++;
                        remain--;
                        
                        evt_times++;
                        
                        evt = (HciEventHdr*)(fw_ptr);
                        fw_ptr += sizeof(*evt);
                        remain -= sizeof(*evt);
                        
                        if (remain < evt->plen) {
                            XYLog("Intel fw corrupted: invalid evt len\n");
                            goto done;
                            break;
                        }
                        
                        evt_param = fw_ptr;
                        fw_ptr += evt->plen;
                        remain -= evt->plen;
                    }
                    /* Every HCI commands in the firmware file has its correspond event.
                     * If event is not found or remain is smaller than zero, the firmware
                     * file is corrupted.
                     */
                    if (!evt || !evt_param || remain < 0) {
                        XYLog("Intel fw corrupted: invalid evt read\n");
                        goto done;
                        break;
                    }
                    if ((ret = sendHCIRequest(USBToHost16(cmd->opcode), cmd->plen, (void *)cmd_param)) != kIOReturnSuccess) {
                        XYLog("sending Intel patch command (0x%4.4x) failed (%d) %s\n",
                              cmd->opcode, ret, stringFromReturn(ret));
                        goto done;
                        break;
                    }
                    for (int j=0; j<evt_times; j++) {
                        interruptPipeRead();
                        IOLockLock(completion);
                        AbsoluteTime deadline;
                        clock_interval_to_deadline(HCI_INIT_TIMEOUT, kMillisecondScale, reinterpret_cast<uint64_t*> (&deadline));
                        int ret = IOLockSleepDeadline(completion, this, deadline, THREAD_INTERRUPTIBLE);
                        IOLockUnlock(completion);
                        if (ret != THREAD_AWAKENED) {
                            goto done;
                            break;
                        }
                    }
                }
                
                mDeviceState = kExitMfg;
                break;
            }
            case kExitMfg:
            {
                //                bool reset = true;
                //                bool patched = true;
                //                uint8_t param[] = { 0x00, 0x00 };
                /* The 2nd command parameter specifies the manufacturing exit method:
                 * 0x00: Just disable the manufacturing mode (0x00).
                 * 0x01: Disable manufacturing mode and reset with patches deactivated.
                 * 0x02: Disable manufacturing mode and reset with patches activated.
                 */
                //                if (reset)
                //                    param[1] |= patched ? 0x02 : 0x01;
                XYLog("HCI_OP_INTEL_EXIT_MFG\n");
                if ((ret = sendHCIRequest(HCI_OP_INTEL_ENTER_MFG, 2, (void *)EXIT_MFG_PARAM)) != kIOReturnSuccess) {
                    XYLog("Exiting manufacturer mode failed (%d) %s\n", ret, stringFromReturn(ret));
                    goto done;
                    break;
                }
                break;
            }
            case kSetEventMask:
            {
                XYLog("HCI_OP_INTEL_EVENT_MASK\n");
                if ((ret = sendHCIRequest(HCI_OP_INTEL_EVENT_MASK, 8, (void *)EVENT_MASK)) != kIOReturnSuccess) {
                    XYLog("Setting Intel event mask failed (%d) %s\n", ret, stringFromReturn(ret));
                    goto done;
                    break;
                }
                isSucceed = true;
                mDeviceState = kUpdateDone;
                break;
            }
            default:
                break;
        }
        
        if (isRequest) {
            if (!interruptPipeRead())
            {
                XYLog("hci read pipe fail. stop\n");
                break;
            }
            XYLog("interrupt wait\n");
            IOLockLock(completion);
            AbsoluteTime deadline;
            clock_interval_to_deadline(HCI_INIT_TIMEOUT, kMillisecondScale, reinterpret_cast<uint64_t*> (&deadline));
            IOLockSleepDeadline(completion, this, deadline, THREAD_INTERRUPTIBLE);
            IOLockUnlock(completion);
        }
        isRequest = false;
        XYLog("interrupt continue\n");
    }
    
done:
    
    publishReg(isSucceed);
    XYLog("End download\n");
}

IOReturn IntelBluetoothFirmware::sendHCIRequest(uint16_t opCode, uint8_t paramLen, const void * param)
{
    isRequest = true;
    //    XYLog("opCode=0x%02x, paramLen=%d\n", opCode, paramLen);
    StandardUSB::DeviceRequest request =
    {
        .bmRequestType = makeDeviceRequestbmRequestType(kRequestDirectionOut, kRequestTypeClass, kRequestRecipientDevice),
        .bRequest = 0,
        .wValue = 0,
        .wIndex = 0,
        .wLength = (uint16_t)(HCI_COMMAND_HDR_SIZE + paramLen)
    };
    uint32_t bytesTransfered;
    bzero(hciCommand, sizeof(HciCommandHdr));
    hciCommand->opcode = opCode;
    hciCommand->plen = paramLen;
    memcpy((void *)hciCommand->pData, param, paramLen);
    IOReturn ret = m_pInterface->deviceRequest(request, (void *)hciCommand, bytesTransfered);
    return ret;
}

IOReturn IntelBluetoothFirmware::bulkWrite(const void *data, uint16_t length)
{
    IOMemoryDescriptor* buffer = IOMemoryDescriptor::withAddress((void*)data, length, kIODirectionOut);
    if (!buffer) {
        XYLog("Unable to allocate bulk write buffer.\n");
        return kIOReturnNoMemory;
    }
    IOReturn ret;
    if ((ret = buffer->prepare(kIODirectionOut)) != kIOReturnSuccess) {
        XYLog("Failed to prepare bulk write memory buffer, %s\n", stringFromReturn(ret));
        buffer->release();
        return ret;
    }
    if ((ret = m_pBulkWritePipe->io(buffer, buffer->getLength(), (IOUSBHostCompletion*)NULL, 0)) != kIOReturnSuccess) {
        XYLog("Failed to write to bulk pipe, %s\n", stringFromReturn(ret));
        buffer->release();
        return ret;
    }
    if ((ret = buffer->complete(kIODirectionOut)) != kIOReturnSuccess) {
        XYLog("Failed to complete bulk write memory buffer, %s\n", stringFromReturn(ret));
        buffer->release();
        return ret;
    }
    return ret;
}

void IntelBluetoothFirmware::onRead(void *owner, void *parameter, IOReturn status, uint32_t bytesTransferred)
{
    IntelBluetoothFirmware* that = (IntelBluetoothFirmware*)owner;
    
    switch (status) {
        case kIOReturnSuccess:
            that->parseHCIResponse(that->mReadBuffer->getBytesNoCopy(), bytesTransferred, NULL, NULL);
            break;
        case kIOReturnNotResponding:
            XYLog("%s not responding\n", __FUNCTION__);
            that->m_pInterruptReadPipe->clearStall(false);
            break;
            
        default:
            XYLog("%s unhandle status (%d) %s)\n", __FUNCTION__, status, that->stringFromReturn(status));
            break;
    }
    
    IOLockWakeup(that->completion, that, false);
}

void IntelBluetoothFirmware::parseHCIResponse(void* response, UInt16 length, void* output, UInt8* outputLength)
{
    HciEventHdr* header = (HciEventHdr*)response;
    //    XYLog("recv event=0x%04x, length=%d\n", header->evt, header->plen);
    if (currentType == kTypeNew) {
        if (header->evt == 0xff && header->plen > 0) {
            BtIntel::printAllByte(response, length + HCI_COMMAND_HDR_SIZE);
            switch (((uint8_t*)response)[2]) {
                    
                case 0x02:
                    XYLog("Notify: Device reboot done\n");
                    break;
                case 0x06:
                    XYLog("Notify: Firmware download done\n");
                    break;
                    
                default:
                    break;
            }
        }
    }
    switch (header->evt) {
        case HCI_EV_CMD_COMPLETE:
            if (currentType == kTypeOld) {
                onHCICommandSucceed((HciResponse*)response, header->plen);
            } else {
                onHCICommandSucceedNew((HciResponse*)response, header->plen);
            }
            break;
            
        default:
            XYLog("can not proceed response\n");
            break;
    }
}

void IntelBluetoothFirmware::onHCICommandSucceed(HciResponse *command, int length)
{
    //    XYLog("recv command opcode=0x%04x\n", command->opcode);
    BtIntel::printAllByte(command, length + HCI_COMMAND_HDR_SIZE);
    switch (command->opcode) {
        case HCI_OP_RESET:
            mDeviceState = kGetIntelVersion;
            break;
        case HCI_OP_INTEL_VERSION:
        {
            ver = (IntelVersion*)((uint8_t*)command + 5);
            snprintf(firmwareName, sizeof(firmwareName),
                     "ibt-hw-%x.%x.%x-fw-%x.%x.%x.%x.%x.bseq",
                     ver->hw_platform, ver->hw_variant, ver->hw_revision,
                     ver->fw_variant,  ver->fw_revision, ver->fw_build_num,
                     ver->fw_build_ww, ver->fw_build_yy);
            /* fw_patch_num indicates the version of patch the device currently
             * have. If there is no patch data in the device, it is always 0x00.
             * So, if it is other than 0x00, no need to patch the device again.
             */
            if (ver->fw_patch_num) {
                XYLog("Intel device is already patched. "
                      "patch num: %02x", ver->fw_patch_num);
                mDeviceState = kSetEventMask;
                break;
            }
            XYLog("Found device firmware %s \n", firmwareName);
            if (!fwData) {
                FwDesc desc = getFWDescByName(firmwareName);
                fwData = OSData::withBytes(desc.var, desc.size);
            }
            XYLog("request firmware success");
            mDeviceState = kEnterMfg;
            break;
        }
        case HCI_OP_INTEL_ENTER_MFG:
        {
            if (mDeviceState == kEnterMfg) {
                mDeviceState = kLoadFW;
            } else {
                mDeviceState = kSetEventMask;
            }
            break;
        }
        case HCI_OP_INTEL_EVENT_MASK:
            mDeviceState = kUpdateDone;
            break;
            
        default:
            break;
    }
}

void IntelBluetoothFirmware::publishReg(bool isSucceed)
{
    setProperty("fw_name", OSString::withCString(firmwareName));
    m_pDevice->setProperty("FirmwareLoaded", isSucceed);
}

#include <IOKit/IOTypes.h>
#include <libkern/OSAtomic.h>

typedef UInt8  u8;
typedef UInt16 u16;
typedef UInt32 u32;
typedef UInt64 u64;

typedef u8 __u8;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;

typedef __u16 __le16;
typedef __u32 __le32;
typedef __u64 __le64;

typedef SInt8  s8;
typedef SInt16 s16;
typedef SInt32 s32;
typedef SInt64 s64;

typedef s8  __s8;
typedef s16 __s16;
typedef s32 __s32;
typedef s64 __s64;

#define le32_to_cpup __le32_to_cpup
#define __force

static inline __u32 __le32_to_cpup(const __le32 *p)
{
    return (__force __u32)*p;
}

static inline u32 get_unaligned_le32(const void *p)
{
    return le32_to_cpup((__le32 *)p);
}

void IntelBluetoothFirmware::beginDownloadNew()
{
    mDeviceState = kNewGetVersion;
    boot_param = 0x00000000;
    bool isSucceed = false;
    while (true) {
        if (mDeviceState == kNewUpdateDone || mDeviceState == kNewUpdateAbort) {
            break;
        }
        
        IOReturn ret;
        switch (mDeviceState) {
            case kNewGetVersion:
            {
                XYLog("HCI_OP_INTEL_VERSION\n");
                if ((ret = sendHCIRequest(HCI_OP_INTEL_VERSION, 0, NULL)) != kIOReturnSuccess) {
                    XYLog("Reading Intel version information failed, ret=%d\n %s", ret, stringFromReturn(ret));
                    goto done;
                    break;
                }
                break;
            }
            case kNewGetBootParams:
            {
                XYLog("HCI_OP_BOOT_PARAMS\n");
                if ((ret = sendHCIRequest(HCI_OP_READ_INTEL_BOOT_PARAMS, 0, NULL)) != kIOReturnSuccess) {
                    XYLog("Reading Intel version boot params failed, ret=%d\n %s", ret, stringFromReturn(ret));
                    goto done;
                    break;
                }
                break;
            }
            case kNewLoadFW:
            {
                uint8_t* fw = (uint8_t*)fwData->getBytesNoCopy();
                /* Start the firmware download transaction with the Init fragment
                 * represented by the 128 bytes of CSS header.
                 */
                int err = 0;
                XYLog("send firmware header\n");
                uint8_t* fw_ptr = fw;
                err = securedSend(0x00, 128, fw_ptr);
                if (err < 0) {
                    XYLog("Failed to send firmware header (%d)\n", err);
                    goto done;
                }
                /* Send the 256 bytes of public key information from the firmware
                 * as the PKey fragment.
                 */
                XYLog("send firmware pkey\n");
                fw_ptr = fw + 128;
                err = securedSend(0x03, 256, fw_ptr);
                if (err < 0) {
                    XYLog("Failed to send firmware pkey (%d)\n", err);
                    goto done;
                }
                /* Send the 256 bytes of signature information from the firmware
                 * as the Sign fragment.
                 */
                XYLog("send firmware signature\n");
                fw_ptr = fw + 388;
                err = securedSend(0x02, 256, fw_ptr);
                if (err < 0) {
                    XYLog("Failed to send firmware signature (%d)\n", err);
                    goto done;
                }
                fw_ptr = fw + 644;
                uint32_t frag_len = 0;
                XYLog("send firmware data\n");
                while (fw_ptr - fw < fwData->getLength()) {
                    FWCommandHdr *cmd = (FWCommandHdr *)(fw_ptr + frag_len);
                    
                    /* Each SKU has a different reset parameter to use in the
                     * HCI_Intel_Reset command and it is embedded in the firmware
                     * data. So, instead of using static value per SKU, check
                     * the firmware data and save it for later use.
                     */
                    if (cmd->opcode == 0xfc0e) {
                        /* The boot parameter is the first 32-bit value
                         * and rest of 3 octets are reserved.
                         */
                        boot_param = get_unaligned_le32(fw_ptr + sizeof(*cmd));
                        
                        XYLog("boot_param=0x%x\n", boot_param);
                    }
                    
                    frag_len += sizeof(*cmd) + cmd->plen;
                    
                    /* The parameter length of the secure send command requires
                     * a 4 byte alignment. It happens so that the firmware file
                     * contains proper Intel_NOP commands to align the fragments
                     * as needed.
                     *
                     * Send set of commands with 4 byte alignment from the
                     * firmware data buffer as a single Data fragement.
                     */
                    if (!(frag_len % 4)) {
                        err = securedSend(0x01, frag_len, fw_ptr);
                        if (err < 0) {
                            XYLog("Failed to send firmware data (%d)\n",
                                  err);
                            goto done;
                        }
                        
                        fw_ptr += frag_len;
                        frag_len = 0;
                    }
                }
                XYLog("send firmware done\n");
                //here waiting for firmware download done notification
//                isRequest = false;
                mDeviceState = kNewIntelReset;
                break;
            }
            case kNewIntelReset:
            {
                XYLog("HCI_OP_INTEL_RESET\n");
                IntelReset *params = (IntelReset *)INTEL_RESET_PARAM;
                params->boot_param = USBToHost16(boot_param);
                if ((ret = sendHCIRequest(HCI_OP_INTEL_RESET_BOOT, sizeof(IntelReset), (void *)INTEL_RESET_PARAM)) != kIOReturnSuccess) {
                    XYLog("Intel reset failed (%d) boot_param=%08x, %s\n", ret, boot_param, stringFromReturn(ret));
                    goto done;
                    break;
                }
                XYLog("Intel reset succeed\n");
                AbsoluteTime deadline;
                interruptPipeRead();
                IOLockLock(completion);
                clock_interval_to_deadline(5000, kMillisecondScale, reinterpret_cast<uint64_t*> (&deadline));
                int ret = IOLockSleepDeadline(completion, this, deadline, THREAD_INTERRUPTIBLE);
                IOLockUnlock(completion);
                if (ret != THREAD_AWAKENED) {
                    XYLog("%s wait for firmware download done timeout\n", __FUNCTION__);
                }
                isRequest = true;
                mDeviceState = kNewSetEventMask;
                break;
            }
            case kNewSetEventMask:
            {
                XYLog("HCI_OP_INTEL_EVENT_MASK\n");
                if ((ret = sendHCIRequest(HCI_OP_INTEL_EVENT_MASK, 8, (void *)EVENT_MASK)) != kIOReturnSuccess) {
                    XYLog("Setting Intel event mask failed (%d) %s\n", ret, stringFromReturn(ret));
                    goto done;
                    break;
                }
                isSucceed = true;
                mDeviceState = kNewUpdateDone;
                break;
            }
            case kNewResetToBL:
            {
                XYLog("HCI_OP_INTEL_RESET_BL\n");
                if ((ret = sendHCIRequest(HCI_OP_INTEL_RESET_BOOT, sizeof(IntelReset), (void *)INTEL_RESET_BL_PARAM)) != kIOReturnSuccess) {
                    XYLog("FW download error recovery failed (%ld), %s\n", ret, stringFromReturn(ret));
                    m_pDevice->reset();
                    goto done;
                    break;
                }
                IOSleep(150);
                //some devices will not re enum controllers after sending RESET_BL command, so reset it again.
                m_pDevice->reset();
                goto done;
                break;
            }
            default:
                break;
        }
        
        if (isRequest) {
            if (!interruptPipeRead())
            {
                XYLog("hci read pipe fail. stop\n");
                break;
            }
            XYLog("interrupt wait\n");
            IOLockLock(completion);
            AbsoluteTime deadline;
            clock_interval_to_deadline(HCI_INIT_TIMEOUT, kMillisecondScale, reinterpret_cast<uint64_t*> (&deadline));
            int ret = IOLockSleepDeadline(completion, this, deadline, THREAD_INTERRUPTIBLE);
            IOLockUnlock(completion);
            if (ret != THREAD_AWAKENED) {
                XYLog("HCI Timeout, retry\n");
                isSucceed = false;
                mDeviceState = kNewResetToBL;
            }
        }
        isRequest = false;
        XYLog("interrupt continue\n");
    }
    
done:
    
    publishReg(isSucceed);
    
    XYLog("End download\n");
}

int IntelBluetoothFirmware::securedSend(uint8_t fragmentType, uint32_t plen, const uint8_t *p)
{
    while (plen > 0) {
        uint8_t cmd_param[253], fragment_len = (plen > 252) ? 252 : plen;
        
        cmd_param[0] = fragmentType;
        memcpy(cmd_param + 1, p, fragment_len);
        
        uint8_t len = fragment_len + 1;
        bzero(hciCommand, sizeof(HciCommandHdr));
        hciCommand->opcode = 0xfc09;
        hciCommand->plen = len;
        memcpy((void *)hciCommand->pData, cmd_param, len);
        if (bulkWrite((void *)hciCommand, len + HCI_COMMAND_HDR_SIZE)  != kIOReturnSuccess) {
            return -1;
        }
        
        bulkPipeRead();
        IOLockLock(completion);
        AbsoluteTime deadline;
        clock_interval_to_deadline(HCI_INIT_TIMEOUT, kMillisecondScale, reinterpret_cast<uint64_t*> (&deadline));
        int ret = IOLockSleepDeadline(completion, this, deadline, THREAD_INTERRUPTIBLE);
        IOLockUnlock(completion);
        if (ret != THREAD_AWAKENED) {
            XYLog("%s timeout\n", __FUNCTION__);
            return -1;
        }
        
        plen -= fragment_len;
        p += fragment_len;
    }
    
    return 1;
}

void IntelBluetoothFirmware::onHCICommandSucceedNew(HciResponse *command, int length)
{
    BtIntel::printAllByte(command, length + HCI_COMMAND_HDR_SIZE);
    switch (command->opcode) {
        case HCI_OP_INTEL_VERSION:
        {
            ver = (IntelVersion *)IOMalloc(sizeof(IntelVersion));
            memcpy(ver, (uint8_t*)command + 5, sizeof(IntelVersion));
            if (ver->hw_platform != 0x37) {
                XYLog("Unsupported Intel hardware platform (%u)",
                      ver->hw_platform);
                mDeviceState = kNewUpdateAbort;
                break;
            }
            switch (ver->hw_variant) {
                case 0x0b:    /* SfP */
                case 0x0c:    /* WsP */
                case 0x11:    /* JfP */
                case 0x12:    /* ThP */
                case 0x13:    /* HrP */
                case 0x14:    /* CcP */
                    break;
                default:
                    XYLog("Unsupported Intel hardware variant (%u)",
                          ver->hw_variant);
                    mDeviceState = kNewUpdateAbort;
                    break;
            }
            BtIntel::printIntelVersion(ver);
            snprintf(firmwareName, 64, "ibt-%u-%u-%u.sfi",
                     ver->hw_variant,
                     ver->hw_revision,
                     ver->fw_revision);
            XYLog("suspect device firmware: %s\n", firmwareName);
            /* The firmware variant determines if the device is in bootloader
             * mode or is running operational firmware. The value 0x06 identifies
             * the bootloader and the value 0x23 identifies the operational
             * firmware.
             *
             * When the operational firmware is already present, then only
             * the check for valid Bluetooth device address is needed. This
             * determines if the device will be added as configured or
             * unconfigured controller.
             *
             * It is not possible to use the Secure Boot Parameters in this
             * case since that command is only available in bootloader mode.
             */
            if (ver->fw_variant == 0x23) {
                mDeviceState = kNewSetEventMask;
                XYLog("firmware had been download.\n");
                break;
            }
            /* If the device is not in bootloader mode, then the only possible
             * choice is to return an error and abort the device initialization.
             */
            if (ver->fw_variant != 0x06) {
                XYLog("Unsupported Intel firmware variant (%u)\n",
                      ver->fw_variant);
                mDeviceState = kNewUpdateAbort;
                break;
            }
            mDeviceState = kNewGetBootParams;
            break;
        }
        case HCI_OP_READ_INTEL_BOOT_PARAMS:
        {
            params = (IntelBootParams*)((uint8_t*)command + 5);
            if (params->status) {
                XYLog("Intel boot parameters command failed (%02x)\n",
                      params->status);
                mDeviceState = kNewUpdateAbort;
                break;
            }
            XYLog("Device revision is %u\n",
                  USBToHost16(params->dev_revid));
            XYLog("Secure boot is %s\n",
                  params->secure_boot ? "enabled" : "disabled");
            XYLog("OTP lock is %s\n",
                  params->otp_lock ? "enabled" : "disabled");
            XYLog("API lock is %s\n",
                  params->api_lock ? "enabled" : "disabled");
            XYLog("Debug lock is %s\n",
                  params->debug_lock ? "enabled" : "disabled");
            XYLog("Minimum firmware build %u week %u %u\n",
                  params->min_fw_build_nn, params->min_fw_build_cw,
                  2000 + params->min_fw_build_yy);
            /* It is required that every single firmware fragment is acknowledged
             * with a command complete event. If the boot parameters indicate
             * that this bootloader does not send them, then abort the setup.
             */
            if (params->limited_cce != 0x00) {
                XYLog("Unsupported Intel firmware loading method (%u)\n",
                      params->limited_cce);
                mDeviceState = kNewUpdateAbort;
                break;
            }
            char fw_name[64];
            int len = sizeof(fw_name);
            switch (ver->hw_variant) {
                case 0x0b:    /* SfP */
                case 0x0c:    /* WsP */
                    snprintf(fw_name, len, "ibt-%u-%u.sfi",
                             ver->hw_variant,
                             params->dev_revid);
                    break;
                case 0x11:    /* JfP */
                case 0x12:    /* ThP */
                case 0x13:    /* HrP */
                case 0x14:    /* CcP */
                    snprintf(fw_name, len, "ibt-%u-%u-%u.sfi",
                             ver->hw_variant,
                             ver->hw_revision,
                             ver->fw_revision);
                    break;
                default:
                    XYLog("Unsupported Intel firmware naming");
                    mDeviceState = kNewUpdateAbort;
                    break;
            }
            if (!fwData) {
                FwDesc desc = getFWDescByName(fw_name);
                fwData = OSData::withBytes(desc.var, desc.size);
            }
            XYLog("Found device firmware: %s\n", fw_name);
            if (fwData->getLength() < 644) {
                XYLog("Invalid size of firmware file (%zu)\n",
                      fwData->getLength());
                mDeviceState = kNewUpdateAbort;
                break;
            }
            if (ver) {
                IOFree(ver, sizeof(IntelVersion));
                ver = NULL;
            }
            mDeviceState = kNewLoadFW;
            break;
        }
        case HCI_OP_INTEL_EVENT_MASK:
            mDeviceState = kNewUpdateDone;
            break;
            
        default:
            break;
    }
}

IOReturn IntelBluetoothFirmware::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
//    XYLog("setPowerState powerStateOrdinal=%lu\n", powerStateOrdinal);
    return IOPMAckImplied;
}

void IntelBluetoothFirmware::stop(IOService *provider)
{
    XYLog("Driver Stop()\n");
    PMstop();
    super::stop(provider);
}

IOService * IntelBluetoothFirmware::probe(IOService *provider, SInt32 *score)
{
    XYLog("Driver Probe()\n");
    if (!super::probe(provider, score)) {
        XYLog("super probe failed\n");
        return NULL;
    }
    m_pDevice = OSDynamicCast(IOUSBHostDevice, provider);
    if (!m_pDevice) {
        XYLog("is not usb device\n");
        return NULL;
    }
    UInt16 vendorID = USBToHost16(m_pDevice->getDeviceDescriptor()->idVendor);
    UInt16 productID = USBToHost16(m_pDevice->getDeviceDescriptor()->idProduct);
    XYLog("name=%s, class=%s, vendorID=0x%04X, productID=0x%04X\n", m_pDevice->getName(), provider->metaClass->getClassName(), vendorID, productID);
    if (productID == 0x07dc || productID == 0x0a2a || productID == 0x0aa7) {
        currentType = kTypeOld;
    } else {
        currentType = kTypeNew;
    }
    m_pDevice = NULL;
    return this;
}
