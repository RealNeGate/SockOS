#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define EFIAPI
#define IN
#define OUT
#define OPTIONAL

#define EFI_FILE_MODE_READ   0x0000000000000001
#define EFI_FILE_MODE_WRITE  0x0000000000000002
#define EFI_FILE_MODE_CREATE 0x8000000000000000

#define EFI_FILE_READ_ONLY   0x0000000000000001
#define EFI_FILE_HIDDEN      0x0000000000000002
#define EFI_FILE_SYSTEM      0x0000000000000004
#define EFI_FILE_RESERVED    0x0000000000000008
#define EFI_FILE_DIRECTORY   0x0000000000000010
#define EFI_FILE_ARCHIVE     0x0000000000000020
#define EFI_FILE_VALID_ATTR  0x0000000000000037

#define EFI_OPEN_PROTOCOL_GET_PROTOCOL 0x00000002

#define EFI_ERRORS           \
X(EFI_NO_ERROR)          \
X(EFI_LOAD_ERROR)        \
X(EFI_INVALID_PARAMETER) \
X(EFI_UNSUPPORTED)       \
X(EFI_BAD_BUFFER_SIZE)   \
X(EFI_BUFFER_TOO_SMALL)  \
X(EFI_NOT_READY)         \
X(EFI_DEVICE_ERROR)      \
X(EFI_WRITE_PROTECTED)

typedef enum {
    #define X(name) name,
    EFI_ERRORS
        #undef X
} EFI_Errors;

typedef void*    EFI_HANDLE;
typedef void*    EFI_EVENT;
typedef size_t   EFI_STATUS;
typedef size_t   EFI_TPL;
typedef u64 EFI_PHYSICAL_ADDRESS;
typedef u64 EFI_VIRTUAL_ADDRESS;

typedef struct {
    u32 Data1;
    u16 Data2;
    u16 Data3;
    u8  Data4[8];
} EFI_GUID;

typedef struct {
    EFI_GUID VendorGuid;
    void*    VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_EVENT  Event;
    EFI_STATUS Status;
    size_t     BufferSize;
    void*      Buffer;
} EFI_FILE_IO_TOKEN;

typedef struct {
    u16 Year;
    u8  Month;
    u8  Day;
    u8  Hour;
    u8  Minute;
    u8  Second;
    u8  Pad1;
    u32 Nanosecond;
    i16  TimeZone;
    u8  Daylight;
    u8  Pad2;
} EFI_TIME;

typedef struct {
    u32 Resolution;
    u32 Accuracy;
    bool     SetsToZero;
} EFI_TIME_CAPABILITIES;

typedef struct {
    u16 ScanCode;
    i16  UnicodeChar;
} EFI_INPUT_KEY;

typedef struct {
    u32             Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    u64             NumberOfPages;
    u64             Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    u8 Type;
    u8 SubType;
    u8 Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

typedef struct {
    EFI_HANDLE AgentHandle;
    EFI_HANDLE ControllerHandle;
    u32   Attributes;
    u32   OpenCount;
} EFI_OPEN_PROTOCOL_INFORMATION_ENTRY;

typedef struct {
    u64 Signature;
    u32 Revision;
    u32 HeaderSize;
    u32 Crc32;
    u32 Reserved;
} EFI_TABLE_HEADER;

typedef enum
{
    TimerCancel,
    TimerPeriodic,
    TimerRelative
} EFI_TIMER_DELAY;

typedef enum
{ EFI_NATIVE_INTERFACE } EFI_INTERFACE_TYPE;

typedef enum
{
    AllHandles,
    ByRegisterNotify,
    ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

typedef enum
{
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown,
    EfiResetPlatformSpecific
} EFI_RESET_TYPE;

typedef struct {
    EFI_GUID CapsuleGuid;
    u32 HeaderSize;
    u32 Flags;
    u32 CapsuleImageSize;
} EFI_CAPSULE_HEADER;

typedef enum
{
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum
{
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    i32 MaxMode;
    i32 Mode;
    i32 Attribute;
    i32 CursorColumn;
    i32 CursorRow;
    bool    CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

struct _SIMPLE_TEXT_OUTPUT_INTERFACE;

typedef EFI_STATUS(EFIAPI* EFI_TEXT_RESET)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE* This, IN bool ExtendedVerification);
typedef EFI_STATUS(EFIAPI* EFI_TEXT_STRING)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE* This, IN i16* String);
typedef EFI_STATUS(EFIAPI* EFI_TEXT_TEST_STRING)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE* This, IN i16* String);
typedef EFI_STATUS(EFIAPI* EFI_TEXT_QUERY_MODE)(IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE* This,
    IN size_t ModeNumber, OUT size_t* Columns, OUT size_t* Rows);
typedef EFI_STATUS(EFIAPI* EFI_TEXT_SET_MODE)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE* This, IN size_t ModeNumber);
typedef EFI_STATUS(EFIAPI* EFI_TEXT_SET_ATTRIBUTE)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE* This, IN size_t Attribute);
typedef EFI_STATUS(EFIAPI* EFI_TEXT_CLEAR_SCREEN)(IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE* This);
typedef EFI_STATUS(EFIAPI* EFI_TEXT_SET_CURSOR_POSITION)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE* This, IN size_t Column, IN size_t Row);
typedef EFI_STATUS(EFIAPI* EFI_TEXT_ENABLE_CURSOR)(
    IN struct _SIMPLE_TEXT_OUTPUT_INTERFACE* This, IN bool Visible);

typedef struct _SIMPLE_TEXT_OUTPUT_INTERFACE {
    EFI_TEXT_RESET               Reset;
    EFI_TEXT_STRING              OutputString;
    EFI_TEXT_TEST_STRING         TestString;
    EFI_TEXT_QUERY_MODE          QueryMode;
    EFI_TEXT_SET_MODE            SetMode;
    EFI_TEXT_SET_ATTRIBUTE       SetAttribute;
    EFI_TEXT_CLEAR_SCREEN        ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR       EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE*     Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

struct _SIMPLE_TEXT_INPUT_INTERFACE;

typedef EFI_STATUS(EFIAPI* EFI_INPUT_RESET)(
    IN struct _SIMPLE_TEXT_INPUT_INTERFACE* This, IN bool ExtendedVerification);
typedef EFI_STATUS(EFIAPI* EFI_INPUT_READ_KEY)(
    IN struct _SIMPLE_TEXT_INPUT_INTERFACE* This, OUT EFI_INPUT_KEY* Key);

typedef struct _SIMPLE_TEXT_INPUT_INTERFACE {
    EFI_INPUT_RESET    Reset;
    EFI_INPUT_READ_KEY ReadKeyStroke;
    EFI_EVENT          WaitForKeys;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef EFI_STATUS(EFIAPI* EFI_GET_TIME)(
    OUT EFI_TIME* Time, OUT EFI_TIME_CAPABILITIES* Capabilities OPTIONAL);
typedef EFI_STATUS(EFIAPI* EFI_SET_TIME)(IN EFI_TIME* Time);
typedef EFI_STATUS(EFIAPI* EFI_GET_WAKEUP_TIME)(
    OUT bool* Enabled, OUT bool* Pending, OUT EFI_TIME* Time);
typedef EFI_STATUS(EFIAPI* EFI_SET_WAKEUP_TIME)(OUT bool Enable, IN EFI_TIME* Time OPTIONAL);
typedef EFI_STATUS(EFIAPI* EFI_SET_VIRTUAL_ADDRESS_MAP)(IN size_t MemoryMapSize,
    IN size_t DescriptorSize, IN size_t DescriptorVersion, IN EFI_MEMORY_DESCRIPTOR* VirtualMap);
typedef EFI_STATUS(EFIAPI* EFI_CONVERT_POINTER)(IN size_t DebugDisposition, IN OUT void** Address);
typedef EFI_STATUS(EFIAPI* EFI_GET_VARIABLE)(IN i16* VariableName, IN EFI_GUID* VendorGuid,
    OUT u32* Attributes OPTIONAL, IN OUT size_t* DataSize, OUT void* Data);
typedef EFI_STATUS(EFIAPI* EFI_GET_NEXT_VARIABLE_NAME)(
    IN size_t* VariableNameSize, IN OUT i16* VariableName, IN OUT EFI_GUID* VendorGuid);
typedef EFI_STATUS(EFIAPI* EFI_SET_VARIABLE)(IN i16* VariableName, IN EFI_GUID* VendorGuid,
    IN u32 Attributes, IN size_t DataSize, IN void* Data);
typedef EFI_STATUS(EFIAPI* EFI_GET_NEXT_HIGH_MONO_COUNT)(OUT u32* HighCount);
typedef EFI_STATUS(EFIAPI* EFI_RESET_SYSTEM)(IN EFI_RESET_TYPE ResetType, IN EFI_STATUS ResetStatus,
    IN u32 DataSize, IN void* ResetData OPTIONAL);
typedef EFI_STATUS(EFIAPI* EFI_UPDATE_CAPSULE)(IN EFI_CAPSULE_HEADER** CapsuleHeaderArray,
    IN size_t CapsuleCount, IN EFI_PHYSICAL_ADDRESS ScatterGatherList OPTIONAL);
typedef EFI_STATUS(EFIAPI* EFI_QUERY_CAPSULE_CAPABILITIES)(
    IN EFI_CAPSULE_HEADER** CapsuleHeaderArray, IN size_t CapsuleCount,
    OUT u64* MaximumCapsuleSize, OUT EFI_RESET_TYPE* ResetType);
typedef EFI_STATUS(EFIAPI* EFI_QUERY_VARIABLE_INFO)(IN u32 Attributes,
    OUT u64* MaximumVariableStorageSize, OUT u64* RemainingVariableStorageSize,
    OUT u64* MaximumVariableSize);

typedef struct {
    EFI_TABLE_HEADER               Hdr;
    EFI_GET_TIME                   GetTime;
    EFI_SET_TIME                   SetTime;
    EFI_GET_WAKEUP_TIME            GetWakeupTime;
    EFI_SET_WAKEUP_TIME            SetWakeupTime;
    EFI_SET_VIRTUAL_ADDRESS_MAP    SetVirtualAddressMap;
    EFI_CONVERT_POINTER            ConvertPointer;
    EFI_GET_VARIABLE               GetVariable;
    EFI_GET_NEXT_VARIABLE_NAME     GetNextVariableName;
    EFI_SET_VARIABLE               SetVariable;
    EFI_GET_NEXT_HIGH_MONO_COUNT   GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM               ResetSystem;
    EFI_UPDATE_CAPSULE             UpdateCapsule;
    EFI_QUERY_CAPSULE_CAPABILITIES QueryCapsuleCapabilities;
    EFI_QUERY_VARIABLE_INFO        QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef void(EFIAPI* EFI_EVENT_NOTIFY)(IN EFI_EVENT Event, IN void* Context);

typedef EFI_TPL(EFIAPI* EFI_RAISE_TPL)(IN EFI_TPL NewTpl);
typedef EFI_TPL(EFIAPI* EFI_RESTORE_TPL)(IN EFI_TPL OldTpl);
typedef EFI_STATUS(EFIAPI* EFI_ALLOCATE_PAGES)(IN EFI_ALLOCATE_TYPE Type,
    IN EFI_MEMORY_TYPE MemoryType, IN size_t Pages, IN OUT EFI_PHYSICAL_ADDRESS* Memory);
typedef EFI_STATUS(EFIAPI* EFI_FREE_PAGES)(IN EFI_PHYSICAL_ADDRESS Memory, IN size_t Pages);
typedef EFI_STATUS(EFIAPI* EFI_GET_MEMORY_MAP)(IN OUT size_t* MemoryMapSize,
    OUT EFI_MEMORY_DESCRIPTOR* MemoryMap, OUT size_t* MapKey, OUT size_t* DescriptorSize,
    OUT u32* DescriptorVersion);
typedef EFI_STATUS(EFIAPI* EFI_ALLOCATE_POOL)(
    IN EFI_MEMORY_TYPE PoolType, IN size_t Size, OUT void** Buffer);
typedef EFI_STATUS(EFIAPI* EFI_FREE_POOL)(IN void* Buffer);
typedef EFI_STATUS(EFIAPI* EFI_CREATE_EVENT)(IN u32 Type, IN EFI_TPL NotifyTpl,
    IN EFI_EVENT_NOTIFY NotifyFunction OPTIONAL, IN void* NotifyContext OPTIONAL,
    OUT EFI_EVENT* Event);
typedef EFI_STATUS(EFIAPI* EFI_CLOSE_EVENT)(IN EFI_EVENT Event);
typedef EFI_STATUS(EFIAPI* EFI_SET_TIMER)(
    IN EFI_EVENT Event, IN EFI_TIMER_DELAY Type, IN u64 TriggerTime);
typedef EFI_STATUS(EFIAPI* EFI_WAIT_FOR_EVENT)(
    IN size_t NumberOfEvents, IN EFI_EVENT* Event, OUT size_t* Index);
typedef EFI_STATUS(EFIAPI* EFI_SIGNAL_EVENT)(IN EFI_EVENT Event);
typedef EFI_STATUS(EFIAPI* EFI_CHECK_EVENT)(IN EFI_EVENT Event);
typedef EFI_STATUS(EFIAPI* EFI_INSTALL_PROTOCOL_INTERFACE)(IN OUT EFI_HANDLE* Handle,
    IN EFI_GUID* Protocol, IN EFI_INTERFACE_TYPE InterfaceType, IN void* Interface);
typedef EFI_STATUS(EFIAPI* EFI_REINSTALL_PROTOCOL_INTERFACE)(
    IN OUT EFI_HANDLE* Handle, IN EFI_GUID* Protocol, IN void* OldInterface, IN void* NewInterface);
typedef EFI_STATUS(EFIAPI* EFI_UNINSTALL_PROTOCOL_INTERFACE)(
    IN OUT EFI_HANDLE* Handle, IN EFI_GUID* Protocol, IN void* Interface);
typedef EFI_STATUS(EFIAPI* EFI_HANDLE_PROTOCOL)(
    IN EFI_HANDLE* Handle, IN EFI_GUID* Protocol, OUT void** Interface);
typedef EFI_STATUS(EFIAPI* EFI_REGISTER_PROTOCOL_NOTIFY)(
    IN EFI_GUID* Protocol, IN EFI_EVENT Event, OUT void** Registration);
typedef EFI_STATUS(EFIAPI* EFI_LOCATE_HANDLE)(IN EFI_LOCATE_SEARCH_TYPE SearchType,
    IN EFI_GUID* Protocol OPTIONAL, IN void* SearchKey OPTIONAL, IN OUT size_t* BufferSize,
    OUT EFI_HANDLE* Buffer);
typedef EFI_STATUS(EFIAPI* EFI_LOCATE_DEVICE_PATH)(
    IN EFI_GUID* Protocol, IN OUT EFI_DEVICE_PATH_PROTOCOL** DevicePath, OUT EFI_HANDLE* Device);
typedef EFI_STATUS(EFIAPI* EFI_INSTALL_CONFIGURATION_TABLE)(IN EFI_GUID* Guid, IN void* Table);
typedef EFI_STATUS(EFIAPI* EFI_IMAGE_LOAD)(IN bool BootPolicy, IN EFI_HANDLE ParentImageHandle,
    IN EFI_DEVICE_PATH_PROTOCOL* DevicePath, IN void* SourceBuffer OPTIONAL, IN size_t SourceSize,
    OUT EFI_HANDLE* ImageHandle);
typedef EFI_STATUS(EFIAPI* EFI_IMAGE_START)(
    IN EFI_HANDLE ImageHandle, OUT size_t* ExitDataSize, OUT i16** ExitData OPTIONAL);
typedef EFI_STATUS(EFIAPI* EFI_EXIT)(IN EFI_HANDLE ImageHandle, IN EFI_STATUS ExitStatus,
    IN size_t ExitDataSize, IN i16* ExitData OPTIONAL);
typedef EFI_STATUS(EFIAPI* EFI_IMAGE_UNLOAD)(IN EFI_HANDLE ImageHandle);
typedef EFI_STATUS(EFIAPI* EFI_EXIT_BOOT_SERVICES)(IN EFI_HANDLE ImageHandle, IN size_t MapKey);
typedef EFI_STATUS(EFIAPI* EFI_GET_NEXT_MONOTONIC_COUNT)(OUT u64* Count);
typedef EFI_STATUS(EFIAPI* EFI_STALL)(IN size_t Microseconds);
typedef EFI_STATUS(EFIAPI* EFI_SET_WATCHDOG_TIMER)(IN size_t Timeout, IN u64 WatchdogCode,
    IN size_t DataSize, IN i16* WatchdogData OPTIONAL);
typedef EFI_STATUS(EFIAPI* EFI_CONNECT_CONTROLLER)(IN EFI_HANDLE ControllerHandle,
    IN EFI_HANDLE* DriverImageHandle OPTIONAL,
    IN EFI_DEVICE_PATH_PROTOCOL* RemainingDevicePath OPTIONAL, IN bool Recursive);
typedef EFI_STATUS(EFIAPI* EFI_DISCONNECT_CONTROLLER)(IN EFI_HANDLE ControllerHandle,
    IN EFI_HANDLE DriverImageHandle OPTIONAL, IN EFI_HANDLE ChildHandle OPTIONAL);
typedef EFI_STATUS(EFIAPI* EFI_OPEN_PROTOCOL)(IN EFI_HANDLE Handle, IN EFI_GUID* Protocol,
    OUT void** Interface OPTIONAL, IN EFI_HANDLE AgentHandle, IN EFI_HANDLE ControllerHandle,
    IN u32 Attributes);
typedef EFI_STATUS(EFIAPI* EFI_CLOSE_PROTOCOL)(IN EFI_HANDLE Handle, IN EFI_GUID* Protocol,
    IN EFI_HANDLE AgentHandle, IN EFI_HANDLE ControllerHandle);
typedef EFI_STATUS(EFIAPI* EFI_OPEN_PROTOCOL_INFORMATION)(IN EFI_HANDLE Handle,
    IN EFI_GUID* Protocol, OUT EFI_OPEN_PROTOCOL_INFORMATION_ENTRY** EntryBuffer,
    OUT size_t* EntryCount);
typedef EFI_STATUS(EFIAPI* EFI_PROTOCOLS_PER_HANDLE)(
    IN EFI_HANDLE Handle, OUT EFI_GUID*** ProtocolBuffer, OUT size_t* ProtocolBufferCount);
typedef EFI_STATUS(EFIAPI* EFI_LOCATE_HANDLE_BUFFER)(IN EFI_LOCATE_SEARCH_TYPE SearchType,
    IN EFI_GUID* Protocol OPTIONAL, IN void* SearchKey OPTIONAL, OUT size_t* NoHandles,
    OUT EFI_HANDLE** Buffer);
typedef EFI_STATUS(EFIAPI* EFI_LOCATE_PROTOCOL)(
    IN EFI_GUID* Protocol, IN void* Registration OPTIONAL, OUT void** Interface);
typedef EFI_STATUS(EFIAPI* EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES)(
    IN OUT EFI_HANDLE* Handle, ...);
typedef EFI_STATUS(EFIAPI* EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES)(
    IN OUT EFI_HANDLE* Handle, ...);
typedef EFI_STATUS(EFIAPI* EFI_CALCULATE_CRC32)(
    IN void* Data, IN size_t DataSize, OUT u32* Crc32);
typedef EFI_STATUS(EFIAPI* EFI_COPY_MEM)(IN void* Destination, IN void* Source, IN size_t Length);
typedef EFI_STATUS(EFIAPI* EFI_SET_MEM)(IN void* Buffer, IN size_t Size, IN u8 Value);
typedef EFI_STATUS(EFIAPI* EFI_CREATE_EVENT_EX)(IN u32 Type, IN EFI_TPL NotifyTPL,
    IN EFI_EVENT_NOTIFY NotifyFunction OPTIONAL, IN const void* NotifyContext OPTIONAL,
    IN const EFI_GUID* EventGroup OPTIONAL, OUT EFI_EVENT* Event);

typedef struct {
    EFI_TABLE_HEADER                           Hdr;
    EFI_RAISE_TPL                              RaiseTPL;
    EFI_RESTORE_TPL                            RestoreTPL;
    EFI_ALLOCATE_PAGES                         AllocatePages;
    EFI_FREE_PAGES                             FreePages;
    EFI_GET_MEMORY_MAP                         GetMemoryMap;
    EFI_ALLOCATE_POOL                          AllocatePool;
    EFI_FREE_POOL                              FreePool;
    EFI_CREATE_EVENT                           CreateEvent;
    EFI_SET_TIMER                              SetTimer;
    EFI_WAIT_FOR_EVENT                         WaitForEvent;
    EFI_SIGNAL_EVENT                           SignalEvent;
    EFI_CLOSE_EVENT                            CloseEvent;
    EFI_CHECK_EVENT                            CheckEvent;
    EFI_INSTALL_PROTOCOL_INTERFACE             InstallProtocolInterface;
    EFI_REINSTALL_PROTOCOL_INTERFACE           ReinstallProtocolInterface;
    EFI_UNINSTALL_PROTOCOL_INTERFACE           UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL                        HandleProtocol;
    void*                                      Reserved;
    EFI_REGISTER_PROTOCOL_NOTIFY               RegisterProtocolNotify;
    EFI_LOCATE_HANDLE                          LocateHandle;
    EFI_LOCATE_DEVICE_PATH                     LocateDevicePath;
    EFI_INSTALL_CONFIGURATION_TABLE            InstallConfigurationTable;
    EFI_IMAGE_LOAD                             LoadImage;
    EFI_IMAGE_START                            StartImage;
    EFI_EXIT                                   Exit;
    EFI_IMAGE_UNLOAD                           UnloadImage;
    EFI_EXIT_BOOT_SERVICES                     ExitBootServices;
    EFI_GET_NEXT_MONOTONIC_COUNT               GetNextMonotonicCount;
    EFI_STALL                                  Stall;
    EFI_SET_WATCHDOG_TIMER                     SetWatchdogTimer;
    EFI_CONNECT_CONTROLLER                     ConnectController;
    EFI_DISCONNECT_CONTROLLER                  DisconnectController;
    EFI_OPEN_PROTOCOL                          OpenProtocol;
    EFI_CLOSE_PROTOCOL                         CloseProtocol;
    EFI_OPEN_PROTOCOL_INFORMATION              OpenProtocolInformation;
    EFI_PROTOCOLS_PER_HANDLE                   ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER                   LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL                        LocateProtocol;
    EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES   InstallMultipleProtocolInterfaces;
    EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES UninstallMultipleProtocolInterfaces;
    EFI_CALCULATE_CRC32                        CalculateCrc32;
    EFI_COPY_MEM                               CopyMem;
    EFI_SET_MEM                                SetMem;
    EFI_CREATE_EVENT_EX                        CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    i16*                         FirmwareVendor;
    u32                         FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL*  ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* StdErr;
    EFI_RUNTIME_SERVICES*            RuntimeServices;
    EFI_BOOT_SERVICES*               BootServices;
    size_t                           NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE*         ConfigurationTables;
} EFI_SYSTEM_TABLE;

#define EFI_ACPI_TABLE_GUID                                                            \
{                                                                                  \
    0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } \
}

#define EFI_ACPI_20_TABLE_GUID                                                         \
{                                                                                  \
    0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } \
}
#define EFI_LOADED_IMAGE_PROTOCOL_GUID                                                 \
{                                                                                  \
    0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } \
}
typedef struct {
    u32                  Revision;
    EFI_HANDLE                ParentHandle;
    EFI_SYSTEM_TABLE*         SystemTable;
    EFI_HANDLE                DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL* FilePath;
    void*                     Reserved;

    u32 LoadOptionsSize;
    void*    LoadOptions;

    void*            ImageBase;
    u64         ImageSize;
    EFI_MEMORY_TYPE  ImageCodeType;
    EFI_MEMORY_TYPE  ImageDataType;
    EFI_IMAGE_UNLOAD Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef EFI_STATUS(EFIAPI* EFI_IMAGE_UNLOAD)(IN EFI_HANDLE ImageHandle);

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID                                              \
{                                                                                  \
    0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } \
}

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    u8 Blue;
    u8 Green;
    u8 Red;
    u8 Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef enum
{
    EfiBltVideoFill,
    EfiBltVideoToBltBuffer,
    EfiBltBufferToVideo,
    EfiGraphicsOutputBltOperationMax,
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

typedef enum
{
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    u32 RedMask;
    u32 GreenMask;
    u32 BlueMask;
    u32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    u32                  Version;
    u32                  HorizontalResolution;
    u32                  VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    u32                  PixelsPerScanline;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    u32                              MaxMode;
    u32                              Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
    size_t                                SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    size_t                                FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef EFI_STATUS(EFIAPI* EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    IN struct _EFI_GRAPHICS_OUTPUT_PROTOCOL* This, IN u32 ModeNumber, OUT size_t* SizeOfInfo,
    OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION** Info);
typedef EFI_STATUS(EFIAPI* EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    IN struct _EFI_GRAPHICS_OUTPUT_PROTOCOL* This, IN u32 ModeNumber);
typedef EFI_STATUS(EFIAPI* EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    IN struct _EFI_GRAPHICS_OUTPUT_PROTOCOL* This, IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL* BltBuffer,
    IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation, IN size_t SourceX, IN size_t SourceY,
    IN size_t DestinationX, IN size_t DestinationY, IN size_t Width, IN size_t Height,
    IN size_t Delta);

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT        Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE*      Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

struct _EFI_FILE_HANDLE;

typedef EFI_STATUS(EFIAPI* EFI_FILE_OPEN)(IN struct _EFI_FILE_HANDLE* File,
    OUT struct _EFI_FILE_HANDLE** NewHandle, IN i16* FileName, IN u64 OpenMode,
    IN u64 Attributes);
typedef EFI_STATUS(EFIAPI* EFI_FILE_CLOSE)(IN struct _EFI_FILE_HANDLE* File);
typedef EFI_STATUS(EFIAPI* EFI_FILE_DELETE)(IN struct _EFI_FILE_HANDLE* File);
typedef EFI_STATUS(EFIAPI* EFI_FILE_READ)(
    IN struct _EFI_FILE_HANDLE* File, IN OUT size_t* BufferSize, OUT void* Buffer);
typedef EFI_STATUS(EFIAPI* EFI_FILE_WRITE)(
    IN struct _EFI_FILE_HANDLE* File, IN OUT size_t* BufferSize, IN void* Buffer);
typedef EFI_STATUS(EFIAPI* EFI_FILE_GET_POSITION)(
    IN struct _EFI_FILE_HANDLE* File, OUT u64* Position);
typedef EFI_STATUS(EFIAPI* EFI_FILE_SET_POSITION)(
    IN struct _EFI_FILE_HANDLE* File, IN u64* Position);
typedef EFI_STATUS(EFIAPI* EFI_FILE_GET_INFO)(IN struct _EFI_FILE_HANDLE* File,
    IN EFI_GUID* InformationType, IN OUT size_t* BufferSize, IN void* Buffer);
typedef EFI_STATUS(EFIAPI* EFI_FILE_SET_INFO)(IN struct _EFI_FILE_HANDLE* File,
    IN EFI_GUID* InformationType, IN size_t BufferSize, IN void* Buffer);
typedef EFI_STATUS(EFIAPI* EFI_FILE_FLUSH)(IN struct _EFI_FILE_HANDLE* File);
typedef EFI_STATUS(EFIAPI* EFI_FILE_OPEN_EX)(IN struct _EFI_FILE_HANDLE* File,
    OUT struct _EFI_FILE_HANDLE** NewHandle, IN i16* FileName, IN u64 OpenMode,
    IN u64 Attributes, IN OUT EFI_FILE_IO_TOKEN* Token);
typedef EFI_STATUS(EFIAPI* EFI_FILE_READ_EX)(
    IN struct _EFI_FILE_HANDLE* File, IN OUT EFI_FILE_IO_TOKEN* Token);
typedef EFI_STATUS(EFIAPI* EFI_FILE_WRITE_EX)(
    IN struct _EFI_FILE_HANDLE* File, IN OUT EFI_FILE_IO_TOKEN* Token);
typedef EFI_STATUS(EFIAPI* EFI_FILE_FLUSH_EX)(
    IN struct _EFI_FILE_HANDLE* File, IN OUT EFI_FILE_IO_TOKEN* Token);

typedef struct _EFI_FILE_HANDLE {
    u64              Revision;
    EFI_FILE_OPEN         Open;
    EFI_FILE_CLOSE        Close;
    EFI_FILE_DELETE       Delete;
    EFI_FILE_READ         Read;
    EFI_FILE_WRITE        Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO     GetInfo;
    EFI_FILE_SET_INFO     SetInfo;
    EFI_FILE_FLUSH        Flush;
    EFI_FILE_OPEN_EX      OpenEx;
    EFI_FILE_READ_EX      ReadEx;
    EFI_FILE_WRITE_EX     WriteEx;
    EFI_FILE_FLUSH_EX     FlushEx;
} EFI_FILE_PROTOCOL;

typedef EFI_FILE_PROTOCOL EFI_FILE;

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID                                            \
{                                                                                   \
    0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } \
}
struct _EFI_FS_HANDLE;

typedef EFI_STATUS(EFIAPI* EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
    IN struct _EFI_FS_HANDLE* This, OUT EFI_FILE_PROTOCOL** Root);

typedef struct _EFI_FS_HANDLE {
    u64                                    Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME OpenVolume;
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

char *efi_errstr(int val) {
    switch (val) {
        #define X(error) \
        case error:      \
        return #error;
        EFI_ERRORS
            #undef X

        default: return "???";
    }
}
