#include "DiskSector.h"
#include <stdio.h>

#define VWIN32_DIOC_DOS_IOCTL     1 // specified MS-DOS device I/O ctl - Interrupt 21h Function 4400h - 4411h
#define VWIN32_DIOC_DOS_INT25     2 // Absolute Disk Read command - Interrupt 25h
#define VWIN32_DIOC_DOS_INT26     3 // Absolute Disk Write command - Interrupt 25h
#define VWIN32_DIOC_DOS_INT13     4 // Interrupt 13h commands
#define VWIN32_DIOC_SIMCTRLC      5 // Simulate Ctrl-C
#define VWIN32_DIOC_DOS_DRIVEINFO 6 // Interrupt 21h Function 730X commands

#define CARRY_FLAG 1

#ifndef INVALID_SET_FILE_POINTER
#define INVALID_SET_FILE_POINTER (DWORD)-1
#endif

#pragma pack(1)
typedef struct _DISKIO {
    DWORD  dwStartSector;   // starting logical sector number
    WORD   wSectors;        // number of sectors
    DWORD  dwBuffer;        // address of read/write buffer
} DISKIO, * PDISKIO;

typedef struct _DIOC_REGISTERS
{
    DWORD reg_EBX;
    DWORD reg_EDX;
    DWORD reg_ECX;
    DWORD reg_EAX;
    DWORD reg_EDI;
    DWORD reg_ESI;
    DWORD reg_Flags;
} DIOC_REGISTERS, * PDIOC_REGISTERS;

#pragma pack()


// -----------------------------------------------------------------------------------
// DiskSector WinNT
//
bool DiskSectorWinNT::Open(char* vol)
{
    char szDrive[10];
    sprintf(szDrive, "\\\\.\\%c:", vol[0]);
    m_hDisk = ::CreateFileA(
        szDrive,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    return m_hDisk != INVALID_HANDLE_VALUE;
}

void DiskSectorWinNT::Close()
{
    if (m_hDisk != INVALID_HANDLE_VALUE)
        ::CloseHandle(m_hDisk);
}

bool DiskSectorWinNT::ReadSector(DWORD sector, char* Buffer, int sectorSize)
{
    DWORD read = 0;

    if (::SetFilePointer(m_hDisk, sector, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return false;

    if (!::ReadFile(m_hDisk, Buffer, sectorSize, &read, NULL))
        return false;
    return true;
}

bool DiskSectorWinNT::WriteSector(DWORD sector, char* Buffer, int sectorSize)
{
    DWORD wrote = 0;

    if (::SetFilePointer(m_hDisk, sector, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        return false;

    if (!::WriteFile(m_hDisk, Buffer, sectorSize, &wrote, NULL))
        return false;

    return true;
}


// -----------------------------------------------------------------------------------
// DiskSector
//
DiskSector::DiskSector()
{
    if (GetVersion() > 0x80000000)
        util = new DiskSectorWin9x;
    else
        util = new DiskSectorWinNT;
}

void DiskSector::Close()
{
    util->Close();
}

bool DiskSector::Open(char* vol)
{
    return util->Open(vol);
}

bool DiskSector::WriteSector(DWORD sector, char* Buffer, int sectorSize)
{
    return util->WriteSector(sector, Buffer, sectorSize);
}

bool DiskSector::ReadSector(DWORD sector, char* Buffer, int sectorSize)
{
    return util->ReadSector(sector, Buffer, sectorSize);
}

DiskSector::~DiskSector()
{
    delete util;
}


// -----------------------------------------------------------------------------------
// DiskSector Win9x
//
bool DiskSectorWin9x::Open(char* vol)
{
    OSVERSIONINFOEX osvi = { 0 };
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);

    m_bOpened = false;

    if (!::GetVersionEx((OSVERSIONINFO*)&osvi))
    {
        osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
        if (!::GetVersionEx((OSVERSIONINFO*)&osvi))
            return false;
    }

    if (osvi.dwPlatformId != VER_PLATFORM_WIN32_WINDOWS)
        return false;


    m_chDrive = toupper(vol[0]);
    m_nDriveNo = m_chDrive - 'A' + 1;

    char temp[10] = { 0 };
    sprintf(temp, "%c:\\", m_chDrive);

    if (::GetDriveTypeA(temp) != DRIVE_FIXED)
        return false;

    m_bW9xOsr2AndAbove = (osvi.dwMajorVersion >= 4 && osvi.dwMinorVersion >= 10)
        ||
        (
            (osvi.dwBuildNumber == 4 && osvi.dwMinorVersion == 0) &&
            (osvi.szCSDVersion[1] == 'C' || osvi.szCSDVersion[1] == 'B')
            );

    m_hVmm32 = ::CreateFileA(
        "\\\\.\\VWIN32",      // name
        0,                      // access mode
        0,                      // share mode
        NULL,           // security descriptor
        0,                   // ho to create
        FILE_FLAG_DELETE_ON_CLOSE, // file attributes
        NULL);          // handle to file with att to copy 

    m_bOpened = (m_hVmm32 != INVALID_HANDLE_VALUE);

    return m_bOpened;
}

void DiskSectorWin9x::Close()
{
    if (m_bOpened)
        ::CloseHandle(m_hVmm32);
    m_bOpened = false;
}


/*------------------------------------------------------------------
ReadLogicalSectors (hDev, bDrive, dwStartSector, wSectors, lpSectBuff)

Purpose:
Reads sectors from a logical drive.  Uses Int 25h.

Parameters:
hDev
Handle of VWIN32

bDrive
The MS-DOS logical drive number. 1 = A, 2 = B, 3 = C, etc.

dwStartSector
The first logical sector to read

wSectors
The number of sectors to read

lpSectBuff
The caller-supplied buffer that will contain the sector data

Return Value:
Returns TRUE if successful, or FALSE if failure.

Comments:
This function does not validate its parameters.
------------------------------------------------------------------*/
bool DiskSectorWin9x::ReadLogicalSectors(HANDLE hDev,
    BYTE   bDrive,
    DWORD  dwStartSector,
    WORD   wSectors,
    LPBYTE lpSectBuff)
{
    BOOL           fResult;
    DWORD          cb;
    DIOC_REGISTERS reg = { 0 };
    DISKIO         dio = { 0 };

    dio.dwStartSector = dwStartSector;
    dio.wSectors = wSectors;
    dio.dwBuffer = (DWORD)lpSectBuff;

    reg.reg_EAX = bDrive - 1;    // Int 25h drive numbers are 0-based. 
    reg.reg_EBX = (DWORD)&dio;   // Drive letter 0 = A, 1 = B  2 = C ect..
    reg.reg_ECX = 0xFFFF;        // use DISKIO struct

    fResult = ::DeviceIoControl(hDev, VWIN32_DIOC_DOS_INT25,
        &reg, sizeof(reg),
        &reg, sizeof(reg), &cb, 0);

    // Determine if the DeviceIoControl call and the read succeeded.
    fResult = fResult && !(reg.reg_Flags & CARRY_FLAG);

    return fResult == TRUE;
}


/*------------------------------------------------------------------
WriteLogicalSectors (hDev, bDrive, dwStartSector, wSectors, lpSectBuff)

Purpose:
Writes sectors to a logical drive. Uses Int 26h

Parameters:
hDev
Handle of VWIN32

bDrive
The MS-DOS logical drive number. 1 = A, 2 = B, 3 = C, etc.

dwStartSector
The first logical sector to write

wSectors
The number of sectors to write

lpSectBuff
The caller-supplied buffer that contains the sector data

Return Value:
Returns TRUE if successful, or FALSE if failure.

Comments:
This function does not validate its parameters.
------------------------------------------------------------------*/
bool DiskSectorWin9x::WriteLogicalSectors(HANDLE hDev,
    BYTE   bDrive,
    DWORD  dwStartSector,
    WORD   wSectors,
    LPBYTE lpSectBuff)
{
    BOOL           fResult;
    DWORD          cb;
    DIOC_REGISTERS reg = { 0 };
    DISKIO         dio = { 0 };

    dio.dwStartSector = dwStartSector;
    dio.wSectors = wSectors;
    dio.dwBuffer = (DWORD)lpSectBuff;

    reg.reg_EAX = bDrive - 1;    // Int 26h drive numbers are 0-based.
    reg.reg_EBX = (DWORD)&dio;
    reg.reg_ECX = 0xFFFF;        // use DISKIO struct

    fResult = ::DeviceIoControl(hDev, VWIN32_DIOC_DOS_INT26,
        &reg, sizeof(reg),
        &reg, sizeof(reg), &cb, 0);

    // Determine if the DeviceIoControl call and the write succeeded.
    fResult = fResult && !(reg.reg_Flags & CARRY_FLAG);

    return fResult == TRUE;
}


/*------------------------------------------------------------------
NewReadSectors(hDev, bDrive, dwStartSector, wSectors, lpSectBuff)

Purpose:
Reads the specified number of sectors into a caller-supplied
buffer. Uses Int 21h function 7305h

Parameters:
hDev
Handle of VWIN32

bDrive
The MS-DOS logical drive number. 0 = default, 1 = A, 2 = B,
3 = C, etc.

dwStartSector
The first sector to read.

wSectors
The number of sectors to read.

lpSectBuff
The caller-supplied buffer to read into.

Return Value:
Returns TRUE if successful, or FALSE if failure.

Comments:
This function does not validate its parameters.  It assumes that
lpSectBuff is allocated by the caller and is large enough to
hold all of the data from all of the sectors being read.
------------------------------------------------------------------*/
bool DiskSectorWin9x::NewReadSectors(HANDLE hDev,
    BYTE   bDrive,
    DWORD  dwStartSector,
    WORD   wSectors,
    LPBYTE lpSectBuff)
{
    BOOL           fResult;
    DWORD          cb;
    DIOC_REGISTERS reg = { 0 };
    DISKIO         dio;

    dio.dwStartSector = dwStartSector;
    dio.wSectors = wSectors;
    dio.dwBuffer = (DWORD)lpSectBuff;

    reg.reg_EAX = 0x7305;   // Ext_ABSDiskReadWrite
    reg.reg_EBX = (DWORD)&dio;
    reg.reg_ECX = -1;
    reg.reg_EDX = bDrive;   // Int 21h, fn 7305h drive numbers are 1-based

    fResult = ::DeviceIoControl(hDev, VWIN32_DIOC_DOS_DRIVEINFO,
        &reg, sizeof(reg),
        &reg, sizeof(reg), &cb, 0);

    // Determine if the DeviceIoControl call and the read succeeded.
    fResult = fResult && !(reg.reg_Flags & CARRY_FLAG);

    return fResult == TRUE;
}


/*------------------------------------------------------------------
NewWriteSectors(hDev, bDrive, dwStartSector, wSectors, lpSectBuff)

Purpose:
Writes the specified number of sectors from a caller-supplied
buffer. Uses Int 21h function 7305h

Parameters:
hDev
Handle of VWIN32

bDrive
The MS-DOS logical drive number. 0 = default, 1 = A, 2 = B,
3 = C, etc.

dwStartSector
The first sector to write.

wSectors
The number of sectors to write.

lpSectBuff
The caller-supplied buffer from which to write.

Return Value:
Returns TRUE if successful, or FALSE if failure.

Comments:
This function does not validate its parameters.  It assumes that
lpSectBuff is allocated by the caller and is large enough to
hold all of the data to be written.
------------------------------------------------------------------*/
bool DiskSectorWin9x::NewWriteSectors(HANDLE hDev,
    BYTE   bDrive,
    DWORD  dwStartSector,
    WORD   wSectors,
    LPBYTE lpSectBuff)
{
    BOOL           fResult;
    DWORD          cb;
    DIOC_REGISTERS reg = { 0 };
    DISKIO         dio;

    dio.dwStartSector = dwStartSector;
    dio.wSectors = wSectors;
    dio.dwBuffer = (DWORD)lpSectBuff;

    reg.reg_EAX = 0x7305;   // Ext_ABSDiskReadWrite
    reg.reg_EBX = (DWORD)&dio;
    reg.reg_ECX = -1;
    reg.reg_EDX = bDrive;   // Int 21h, fn 7305h drive numbers are 1-based

    reg.reg_ESI = 0x6001;   // Normal file data/write (See function
    // documentation for other values)


    fResult = ::DeviceIoControl(hDev, VWIN32_DIOC_DOS_DRIVEINFO,
        &reg, sizeof(reg),
        &reg, sizeof(reg), &cb, 0);

    // Determine if the DeviceIoControl call and the write succeeded.
    fResult = fResult && !(reg.reg_Flags & CARRY_FLAG);

    return fResult == TRUE;
}

/*-----------------------------------------------------------------------
LockLogicalVolume (hVWin32, bDriveNum, bLockLevel, wPermissions)

Purpose:
Takes a logical volume lock on a logical volume.

Parameters:
hVWin32
An open handle to VWIN32.

bDriveNum
The logical drive number to lock. 0 = default, 1 = A:, 2 = B:,
3 = C:, etc.

bLockLevel
Can be 0, 1, 2, or 3. Level 0 is an exclusive lock that can only
be taken when there are no open files on the specified drive.
Levels 1 through 3 form a hierarchy where 1 must be taken before
2, which must be taken before 3.

wPermissions
Specifies how the lock will affect file operations when lock levels
1 through 3 are taken. Also specifies whether a formatting lock
should be taken after a level 0 lock.

Zero is a valid permission.

Return Value:
If successful, returns TRUE.  If unsuccessful, returns FALSE.
-----------------------------------------------------------------------*/
bool DiskSectorWin9x::LockLogicalVolume(HANDLE hVWin32,
    BYTE   bDriveNum,
    BYTE   bLockLevel,
    WORD   wPermissions)
{
    BOOL           fResult;
    DIOC_REGISTERS regs = { 0 };
    BYTE           bDeviceCat;  // can be either 0x48 or 0x08
    DWORD          cb;

    /*
    Try first with device category 0x48 for FAT32 volumes. If it
    doesn't work, try again with device category 0x08. If that
    doesn't work, then the lock failed.
    */

    bDeviceCat = 0x48;

ATTEMPT_AGAIN:
    // Set up the parameters for the call.
    regs.reg_EAX = 0x440D;
    regs.reg_EBX = MAKEWORD(bDriveNum, bLockLevel);
    regs.reg_ECX = MAKEWORD(0x4A, bDeviceCat);
    regs.reg_EDX = wPermissions;

    fResult = ::DeviceIoControl(hVWin32, VWIN32_DIOC_DOS_IOCTL,
        &regs, sizeof(regs), &regs, sizeof(regs),
        &cb, 0);

    // See if DeviceIoControl and the lock succeeded
    fResult = fResult && !(regs.reg_Flags & CARRY_FLAG);

    // If DeviceIoControl or the lock failed, and device category 0x08
    // hasn't been tried, retry the operation with device category 0x08.
    if (!fResult && (bDeviceCat != 0x08))
    {
        bDeviceCat = 0x08;
        goto ATTEMPT_AGAIN;
    }

    return fResult == TRUE;
}

/*-----------------------------------------------------------------------
UnlockLogicalVolume (hVWin32, bDriveNum)

Purpose:
Unlocks a logical volume that was locked with LockLogicalVolume().

Parameters:
hVWin32
An open handle to VWIN32.

bDriveNum
The logical drive number to unlock. 0 = default, 1 = A:, 2 = B:,
3 = C:, etc.

Return Value:
If successful, returns TRUE. If unsuccessful, returns FALSE.

Comments:
Must be called the same number of times as LockLogicalVolume() to
completely unlock a volume.

Only the lock owner can unlock a volume.
-----------------------------------------------------------------------*/
bool DiskSectorWin9x::UnlockLogicalVolume(HANDLE hVWin32, BYTE bDriveNum)
{
    BOOL           fResult;
    DIOC_REGISTERS regs = { 0 };
    BYTE           bDeviceCat;  // can be either 0x48 or 0x08
    DWORD          cb;

    /* Try first with device category 0x48 for FAT32 volumes. If it
    doesn't work, try again with device category 0x08. If that
    doesn't work, then the unlock failed.
    */

    bDeviceCat = 0x48;

ATTEMPT_AGAIN:
    // Set up the parameters for the call.
    regs.reg_EAX = 0x440D;
    regs.reg_EBX = bDriveNum;
    regs.reg_ECX = MAKEWORD(0x6A, bDeviceCat);

    fResult = ::DeviceIoControl(hVWin32, VWIN32_DIOC_DOS_IOCTL,
        &regs, sizeof(regs), &regs, sizeof(regs),
        &cb, 0);

    // See if DeviceIoControl and the unlock succeeded
    fResult = fResult && !(regs.reg_Flags & CARRY_FLAG);

    // If DeviceIoControl or the unlock failed, and device category 0x08
    // hasn't been tried, retry the operation with device category 0x08.
    if (!fResult && (bDeviceCat != 0x08))
    {
        bDeviceCat = 0x08;
        goto ATTEMPT_AGAIN;
    }
    return fResult == TRUE;
}


bool DiskSectorWin9x::ReadSector(DWORD sector, char* Buffer, int sectorSize)
{
    if (!m_bOpened)
        return false;

    if (m_bUseLocking)
    {
        if (!LockLogicalVolume(m_hVmm32, m_nDriveNo, 1, 1))
            return false;

        if (!LockLogicalVolume(m_hVmm32, m_nDriveNo, 2, 0))
        {
            UnlockLogicalVolume(m_hVmm32, m_nDriveNo);
            return false;
        }
    }

    bool bRet;
    if (m_bW9xOsr2AndAbove)
        bRet = NewReadSectors(m_hVmm32, m_nDriveNo, sector, 1, (LPBYTE)Buffer);
    else
        bRet = ReadLogicalSectors(m_hVmm32, m_nDriveNo, sector, 1, (LPBYTE)Buffer);

    if (m_bUseLocking)
    {
        UnlockLogicalVolume(m_hVmm32, m_nDriveNo);
        UnlockLogicalVolume(m_hVmm32, m_nDriveNo);
    }
    return bRet;
}

bool DiskSectorWin9x::WriteSector(DWORD sector, char* Buffer, int sectorSize)
{
    if (!m_bOpened)
        return false;

    if (!LockLogicalVolume(m_hVmm32, m_nDriveNo, 1, 1))
        return false;

    if (!LockLogicalVolume(m_hVmm32, m_nDriveNo, 2, 0))
    {
        UnlockLogicalVolume(m_hVmm32, m_nDriveNo);
        return false;
    }

    if (!LockLogicalVolume(m_hVmm32, m_nDriveNo, 3, 0))
    {
        UnlockLogicalVolume(m_hVmm32, m_nDriveNo);
        UnlockLogicalVolume(m_hVmm32, m_nDriveNo);
        return false;
    }

    bool bRet;

    if (m_bW9xOsr2AndAbove)
        bRet = NewWriteSectors(m_hVmm32, m_nDriveNo, sector, 1, (LPBYTE)Buffer);
    else
        bRet = WriteLogicalSectors(m_hVmm32, m_nDriveNo, sector, 1, (LPBYTE)Buffer);

    UnlockLogicalVolume(m_hVmm32, m_nDriveNo);
    UnlockLogicalVolume(m_hVmm32, m_nDriveNo);
    UnlockLogicalVolume(m_hVmm32, m_nDriveNo);

    return bRet;
}
