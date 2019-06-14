/**
 * @file winfuse/fuse.c
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

NTSTATUS FuseDeviceInit(PDEVICE_OBJECT DeviceObject);
VOID FuseDeviceFini(PDEVICE_OBJECT DeviceObject);
VOID FuseDeviceExpirationRoutine(PDEVICE_OBJECT DeviceObject, UINT64 ExpirationTime);
NTSTATUS FuseDeviceTransact(PIRP Irp, PDEVICE_OBJECT DeviceObject);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FuseDeviceInit)
#pragma alloc_text(PAGE, FuseDeviceFini)
#pragma alloc_text(PAGE, FuseDeviceExpirationRoutine)
#pragma alloc_text(PAGE, FuseDeviceTransact)
#pragma alloc_text(PAGE, FuseContextCreate)
#pragma alloc_text(PAGE, FuseContextDelete)
#pragma alloc_text(PAGE, FuseNtStatusFromErrno)
#endif

NTSTATUS FuseDeviceInit(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    FUSE_DEVICE_EXTENSION *DeviceExtension = FuseDeviceExtension(DeviceObject);
    NTSTATUS Result;

    /* on failure FuseDeviceFini will be called to cleanup */

    Result = FuseIoqCreate((FUSE_IOQ **)&DeviceExtension->Ioq);
    if (!NT_SUCCESS(Result))
        return Result;

    Result = FuseCacheCreate(0, !DeviceExtension->VolumeParams->CaseSensitiveSearch,
        (FUSE_CACHE **)&DeviceExtension->Cache);
    if (!NT_SUCCESS(Result))
        return Result;

    KeInitializeEvent(&DeviceExtension->InitEvent, NotificationEvent, FALSE);

    return STATUS_SUCCESS;
}

VOID FuseDeviceFini(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    FUSE_DEVICE_EXTENSION *DeviceExtension = FuseDeviceExtension(DeviceObject);

    if (0 != DeviceExtension->Cache)
        FuseCacheDelete(DeviceExtension->Cache);

    if (0 != DeviceExtension->Ioq)
        FuseIoqDelete(DeviceExtension->Ioq);
}

VOID FuseDeviceExpirationRoutine(PDEVICE_OBJECT DeviceObject, UINT64 ExpirationTime)
{
    PAGED_CODE();

    FUSE_DEVICE_EXTENSION *DeviceExtension = FuseDeviceExtension(DeviceObject);

    FuseCacheInvalidateExpired(DeviceExtension->Cache, ExpirationTime, DeviceObject);
}

FUSE_PROCESS_DISPATCH *FuseProcessFunction[FspFsctlTransactKindCount];

static inline BOOLEAN FuseContextProcess(FUSE_CONTEXT *Context,
    FUSE_PROTO_RSP *FuseResponse, FUSE_PROTO_REQ *FuseRequest)
{
    UINT32 Kind = 0 == Context->InternalRequest ?
        FspFsctlTransactReservedKind : Context->InternalRequest->Kind;

    Context->FuseRequest = FuseRequest;
    Context->FuseResponse = FuseResponse;

    return FuseProcessFunction[Kind](Context);
}

NTSTATUS FuseDeviceTransact(PIRP Irp, PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ASSERT(IRP_MJ_FILE_SYSTEM_CONTROL == IrpSp->MajorFunction);
    ASSERT(IRP_MN_USER_FS_REQUEST == IrpSp->MinorFunction);
    ASSERT(FSP_FSCTL_TRANSACT_FUSE == IrpSp->Parameters.FileSystemControl.FsControlCode);
    ASSERT(METHOD_BUFFERED == (IrpSp->Parameters.FileSystemControl.FsControlCode & 3));
    ASSERT(IrpSp->FileObject->FsContext2 == DeviceObject);

    /* check parameters */
    ULONG InputBufferLength = IrpSp->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutputBufferLength = IrpSp->Parameters.FileSystemControl.OutputBufferLength;
    FUSE_PROTO_RSP *FuseResponse = 0 != InputBufferLength ? Irp->AssociatedIrp.SystemBuffer : 0;
    FUSE_PROTO_REQ *FuseRequest = 0 != OutputBufferLength ? Irp->AssociatedIrp.SystemBuffer : 0;
    if (0 != FuseResponse)
    {
        if (FUSE_PROTO_RSP_HEADER_SIZE > InputBufferLength ||
            FUSE_PROTO_RSP_HEADER_SIZE > FuseResponse->len ||
            FuseResponse->len > InputBufferLength)
            return STATUS_INVALID_PARAMETER;
    }
    if (0 != FuseRequest)
    {
        if (FUSE_PROTO_REQ_SIZEMIN > OutputBufferLength)
            return STATUS_BUFFER_TOO_SMALL;
    }

    FUSE_DEVICE_EXTENSION *DeviceExtension = FuseDeviceExtension(DeviceObject);
    FSP_FSCTL_TRANSACT_REQ *InternalRequest = 0;
    FSP_FSCTL_TRANSACT_RSP InternalResponse;
    FUSE_CONTEXT *Context;
    BOOLEAN Continue;
    NTSTATUS Result;

    if (0 != FuseResponse)
    {
        Context = FuseIoqEndProcessing(DeviceExtension->Ioq, FuseResponse->unique);
        if (0 == Context)
            goto request;

        Continue = FuseContextProcess(Context, FuseResponse, 0);

        if (Continue)
            FuseIoqPostPending(DeviceExtension->Ioq, Context);
        else if (0 == Context->InternalRequest)
            FuseContextDelete(Context);
        else
        {
            ASSERT(FspFsctlTransactReservedKind != Context->InternalResponse->Kind);

            Result = FuseSendTransactInternalIrp(
                DeviceObject, IrpSp->FileObject, Context->InternalResponse, 0);
            FuseContextDelete(Context);
            if (!NT_SUCCESS(Result))
                goto exit;
        }
    }

request:
    if (0 != FuseRequest)
    {
        RtlZeroMemory(FuseRequest, FUSE_PROTO_REQ_HEADER_SIZE);

        Context = FuseIoqNextPending(DeviceExtension->Ioq);
        if (0 == Context)
        {
            UINT32 VersionMajor = DeviceExtension->VersionMajor;
            MemoryBarrier();
            if (0 == VersionMajor)
            {
                Result = FsRtlCancellableWaitForSingleObject(&DeviceExtension->InitEvent,
                    0, Irp);
                if (STATUS_TIMEOUT == Result || STATUS_THREAD_IS_TERMINATING == Result)
                    Result = STATUS_CANCELLED;
                if (!NT_SUCCESS(Result))
                    goto exit;
                ASSERT(STATUS_SUCCESS == Result);

                VersionMajor = DeviceExtension->VersionMajor;
            }
            if ((UINT32)-1 == VersionMajor)
            {
                Result = STATUS_ACCESS_DENIED;
                goto exit;
            }

            Result = FuseSendTransactInternalIrp(
                DeviceObject, IrpSp->FileObject, 0, &InternalRequest);
            if (!NT_SUCCESS(Result))
                goto exit;
            if (0 == InternalRequest)
            {
                Irp->IoStatus.Information = 0;
                Result = STATUS_SUCCESS;
                goto exit;
            }

            ASSERT(FspFsctlTransactReservedKind != InternalRequest->Kind);

            FuseContextCreate(&Context, DeviceObject, InternalRequest);
            ASSERT(0 != Context);

            Continue = FALSE;
            if (!FuseContextIsStatus(Context))
            {
                InternalRequest = 0;
                Continue = FuseContextProcess(Context, 0, FuseRequest);
            }
        }
        else
            Continue = FuseContextProcess(Context, 0, FuseRequest);

        if (Continue)
        {
            ASSERT(!FuseContextIsStatus(Context));
            FuseIoqStartProcessing(DeviceExtension->Ioq, Context);
        }
        else if (0 == Context->InternalRequest)
            /* ignore */;
        else
        {
            if (FuseContextIsStatus(Context))
            {
                RtlZeroMemory(&InternalResponse, sizeof InternalResponse);
                InternalResponse.Size = sizeof InternalResponse;
                InternalResponse.Kind = InternalRequest->Kind;
                InternalResponse.Hint = InternalRequest->Hint;
                InternalResponse.IoStatus.Status = FuseContextToStatus(Context);
                Result = FuseSendTransactInternalIrp(
                    DeviceObject, IrpSp->FileObject, &InternalResponse, 0);
            }
            else
            {
                Result = FuseSendTransactInternalIrp(
                    DeviceObject, IrpSp->FileObject, Context->InternalResponse, 0);
                FuseContextDelete(Context);
            }

            if (!NT_SUCCESS(Result))
                goto exit;
        }

        Irp->IoStatus.Information = FuseRequest->len;
    }
    else
        Irp->IoStatus.Information = 0;

    Result = STATUS_SUCCESS;

exit:
    if (0 != InternalRequest)
        FuseFree(InternalRequest);

    return Result;
}

VOID FuseContextCreate(FUSE_CONTEXT **PContext,
    PDEVICE_OBJECT DeviceObject, FSP_FSCTL_TRANSACT_REQ *InternalRequest)
{
    PAGED_CODE();

    FUSE_CONTEXT *Context;
    UINT32 Kind = 0 == InternalRequest ?
        FspFsctlTransactReservedKind : InternalRequest->Kind;

    ASSERT(FspFsctlTransactKindCount > Kind);
    if (0 == FuseProcessFunction[Kind])
    {
        *PContext = FuseContextStatus(STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    Context = FuseAlloc(sizeof *Context);
    if (0 == Context)
    {
        *PContext = FuseContextStatus(STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    RtlZeroMemory(Context, sizeof *Context);
    Context->DeviceObject = DeviceObject;
    Context->InternalRequest = InternalRequest;
    Context->InternalResponse = (PVOID)&Context->InternalResponseBuf;
    Context->InternalResponse->Size = sizeof(FSP_FSCTL_TRANSACT_RSP);
    Context->InternalResponse->Kind = Kind;
    Context->InternalResponse->Hint = 0 != InternalRequest ? InternalRequest->Hint : 0;
    *PContext = Context;
}

VOID FuseContextDelete(FUSE_CONTEXT *Context)
{
    PAGED_CODE();

    if (0 != Context->Fini)
        Context->Fini(Context);
    if (0 != Context->InternalRequest)
        FuseFree(Context->InternalRequest);
    if ((PVOID)&Context->InternalResponseBuf != Context->InternalResponse)
        FuseFree(Context->InternalResponse);
    FuseFree(Context);
}

NTSTATUS FuseNtStatusFromErrno(INT32 Errno)
{
    PAGED_CODE();

    switch (Errno)
    {
    #undef FUSE_ERRNO
    #define FUSE_ERRNO 87
    #include <winfuse/errno.i>
    default:
        return STATUS_ACCESS_DENIED;
    }
}