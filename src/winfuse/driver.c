/**
 * @file winfuse/driver.c
 *
 * @copyright 2019 Bill Zissimopoulos
 */
/*
 * This file is part of WinFuse.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * Affero General Public License version 3 as published by the Free
 * Software Foundation.
 *
 * Licensees holding a valid commercial license may use this software
 * in accordance with the commercial license agreement provided in
 * conjunction with the software.  The terms and conditions of any such
 * commercial license agreement shall govern, supersede, and render
 * ineffective any application of the AGPLv3 license to this software,
 * notwithstanding of any reference thereto in the software or
 * associated repository.
 */

#include <winfuse/driver.h>

DRIVER_INITIALIZE DriverEntry;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

NTSTATUS DriverEntry(
    PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    FuseProcessFunction[FspFsctlTransactReservedKind] = FuseOpReserved;
    FuseProcessFunction[FspFsctlTransactCreateKind] = FuseOpCreate;
    //FuseProcessFunction[FspFsctlTransactOverwriteKind] = FuseOpOverwrite;
    //FuseProcessFunction[FspFsctlTransactCleanupKind] = FuseOpCleanup;
    //FuseProcessFunction[FspFsctlTransactCloseKind] = FuseOpClose;
    //FuseProcessFunction[FspFsctlTransactReadKind] = FuseOpRead;
    //FuseProcessFunction[FspFsctlTransactWriteKind] = FuseOpWrite;
    //FuseProcessFunction[FspFsctlTransactQueryInformationKind] = FuseOpQueryInformation;
    //FuseProcessFunction[FspFsctlTransactSetInformationKind] = FuseOpSetInformation;
    //FuseProcessFunction[FspFsctlTransactQueryEaKind] = FuseOpQueryEa;
    //FuseProcessFunction[FspFsctlTransactSetEaKind] = FuseOpSetEa;
    //FuseProcessFunction[FspFsctlTransactFlushBuffersKind] = FuseOpFlushBuffers;
    //FuseProcessFunction[FspFsctlTransactQueryVolumeInformationKind] = FuseOpQueryVolumeInformation;
    //FuseProcessFunction[FspFsctlTransactSetVolumeInformationKind] = FuseOpSetVolumeInformation;
    //FuseProcessFunction[FspFsctlTransactQueryDirectoryKind] = FuseOpQueryDirectory;
    //FuseProcessFunction[FspFsctlTransactFileSystemControlKind] = FuseOpFileSystemControl;
    //FuseProcessFunction[FspFsctlTransactDeviceControlKind] = FuseOpDeviceControl;
    //FuseProcessFunction[FspFsctlTransactQuerySecurityKind] = FuseOpQuerySecurity;
    //FuseProcessFunction[FspFsctlTransactSetSecurityKind] = FuseOpSetSecurity;
    //FuseProcessFunction[FspFsctlTransactQueryStreamInformationKind] = FuseOpQueryStreamInformation;

    static FSP_FSEXT_PROVIDER Provider;
    Provider.Version = sizeof Provider;
    Provider.DeviceTransactCode = FSP_FSCTL_TRANSACT_FUSE;
    Provider.DeviceExtensionSize = sizeof(FUSE_DEVICE_EXTENSION);
    Provider.DeviceInit = FuseDeviceInit;
    Provider.DeviceFini = FuseDeviceFini;
    Provider.DeviceExpirationRoutine = FuseDeviceExpirationRoutine;
    Provider.DeviceTransact = FuseDeviceTransact;
    return FspFsextRegisterProvider(&Provider);
}