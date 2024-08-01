#include <ntifs.h>
#include <wdmsec.h> // IoCreateDeviceSecure
#include <TraceLoggingProvider.h>
#include <evntrace.h>

#pragma warning(disable : 4996)
#pragma warning(disable : 4100)

/* ! I developed this driver as a quick solution for a book project.  It provided the correct results. 
	 However, I am not a kernel programming expert, I'm still learning, so please be aware that the code below may contain errors.
*/

/*
The driver DPCenumerator is designed to enumerate Deferred Procedure Calls (DPCs) on each processor of a multi-processor system. 
Key Features:

- No Undocumented Structures
*/


enum class DpcType {
	Dpc_Normal,
	Dpc_Threaded
};

// {73EBA235-AF00-475C-BBD9-4CD08E423D93}
TRACELOGGING_DEFINE_PROVIDER( g_Provider , "DPCenumerator", (0x73eba235 , 0xaf00 , 0x475c , 0xbb , 0xd9 , 0x4c , 0xd0 , 0x8e , 0x42 , 0x3d , 0x93 ));

#define IOCTL_ENUM_DPC CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define SYMBOLIC_NAME L"\\??\\DPCenumerator"
#define DEVICE_NAME L"\\Device\\DPCenumerator"


void DriverUnload( _In_ PDRIVER_OBJECT DriverObject );
NTSTATUS DriverDispatchCreateClose( _In_ PDEVICE_OBJECT DeviceObject , _Inout_  PIRP Irp );
NTSTATUS DriverDispatchControl( _In_ PDEVICE_OBJECT DeviceObject , _Inout_  PIRP Irp );
void EnumerateDPCs( _In_ PKDPC &Dpc );
PKDPC InsertQueueImportantDpc( DpcType Type );

UNICODE_STRING gSymbolicName;

#pragma region Routine
VOID DpcDummyRoutine(
	_In_ struct _KDPC *Dpc ,
	_In_opt_ PVOID DeferredContext ,
	_In_opt_ PVOID SystemArgument1 ,
	_In_opt_ PVOID SystemArgument2
) {
	ExFreePool( Dpc );
}
#pragma endregion Routine


extern "C" NTSTATUS DriverEntry( PDRIVER_OBJECT DriverObject , PUNICODE_STRING RegisteryPath ) {

	TraceLoggingRegister( g_Provider );

	NTSTATUS status;
	UNICODE_STRING deviceName;
	UNICODE_STRING SDDLstring;
	PDEVICE_OBJECT DeviceObject;


	DriverObject->DriverUnload = DriverUnload;

	RtlInitUnicodeString( &deviceName , DEVICE_NAME );
	RtlInitUnicodeString( &gSymbolicName , SYMBOLIC_NAME );
	RtlInitUnicodeString( &SDDLstring , L"D:P(A;;GA;;;SY)(A;;GRGWGX;;;BA)" ); // SYSTEM + Administrators only 

	status = IoCreateDeviceSecure( DriverObject , 0 , &deviceName , FILE_DEVICE_UNKNOWN , FILE_DEVICE_SECURE_OPEN , false , &SDDLstring , nullptr , &DeviceObject );

	if ( !NT_SUCCESS( status ) ) return status;

	status = IoCreateSymbolicLink( &gSymbolicName , &deviceName );

	if ( !NT_SUCCESS( status ) ) {

		IoDeleteDevice( DriverObject->DeviceObject );
		return status;
	}


	DriverObject->MajorFunction [IRP_MJ_CREATE] = &DriverDispatchCreateClose;
	DriverObject->MajorFunction [IRP_MJ_CLOSE] = &DriverDispatchCreateClose;
	DriverObject->MajorFunction [IRP_MJ_DEVICE_CONTROL] = &DriverDispatchControl;


	return STATUS_SUCCESS;
}

void DriverUnload( _In_ PDRIVER_OBJECT DriverObject ) {

	IoDeleteSymbolicLink( &gSymbolicName );
	IoDeleteDevice( DriverObject->DeviceObject );
	TraceLoggingUnregister( g_Provider );

	return;
}


NTSTATUS DriverDispatchCreateClose( _In_ PDEVICE_OBJECT DeviceObject , _Inout_  PIRP Irp ) {

	UNREFERENCED_PARAMETER( DeviceObject );

	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = NULL;

	IoCompleteRequest( Irp , 0 );

	return STATUS_SUCCESS;

}

NTSTATUS DriverDispatchControl( _In_ PDEVICE_OBJECT DeviceObject , _Inout_  PIRP Irp ) {

	NTSTATUS status { STATUS_SUCCESS };
	constexpr ULONG_PTR information { };

	auto pIrpStackLocation = IoGetCurrentIrpStackLocation( Irp );

	switch ( pIrpStackLocation->Parameters.DeviceIoControl.IoControlCode ) {
	case IOCTL_ENUM_DPC:
	{

		KIRQL Irql;

		KAFFINITY ActiveProcBitmask = KeQueryActiveProcessors( );

		for ( ULONG ProcNumber = 0; ( ActiveProcBitmask >> ProcNumber ) & 0x01; ProcNumber++ ) {

			// Change the processor to the specified one 
			KeSetSystemAffinityThread( ( KAFFINITY ) ( ( ULONG_PTR ) 1 << ProcNumber ) );
			
		    // Raise to Dispatch_level preventing changes to the DPC queue on the current processor
		    KeRaiseIrql( DISPATCH_LEVEL , &Irql );
			
			// Insert Dummys DPCs into the target processor
			PKDPC Dpc = InsertQueueImportantDpc( DpcType::Dpc_Normal );
			PKDPC DpcThreaded = InsertQueueImportantDpc( DpcType::Dpc_Threaded );

			// Iterate over the DpcListEntry
			if(Dpc) EnumerateDPCs( Dpc );
			if ( DpcThreaded ) EnumerateDPCs( DpcThreaded );

			// back to PASSIVE_LEVEL, allowing Dpc's DeferredRoutine to execute on the current processor and to free the allocated DPC
			KeLowerIrql( Irql );
			
		}
		
		
		 // Restore the original affinity
		KeRevertToUserAffinityThread( );
		

		break;

	}
	default:
	{
		status = STATUS_INVALID_DEVICE_REQUEST;
		break;
	}
	}


	Irp->IoStatus.Status = status;
	Irp->IoStatus.Information = information;
	IoCompleteRequest( Irp , IO_NO_INCREMENT );


	return status;
}

PKDPC InsertQueueImportantDpc( DpcType Type ) 	{

	// Allocate non-paged memory for the DPC
	PKDPC Dpc = reinterpret_cast< PKDPC >( ExAllocatePoolWithTag( NonPagedPoolNx , sizeof( KDPC ) , 'FloF' ) );

	if ( Dpc == NULL ) return Dpc;

	if ( Type == DpcType::Dpc_Normal )
		KeInitializeDpc( Dpc , DpcDummyRoutine , nullptr );
	else
		KeInitializeThreadedDpc( Dpc , DpcDummyRoutine , nullptr );


	// Insert Dpc at the head of the queue
	// This is necessary because iterating over the singled linked list DpcListEntry is not possible when the DPC is at the tail of the queue.
	KeSetImportanceDpc( Dpc , HighImportance );


	bool Inserted = KeInsertQueueDpc( Dpc , nullptr , nullptr );

	// The DPC is not inserted, free the allocated DPC
	if ( !Inserted ) 		{
		ExFreePool( Dpc );
		Dpc = nullptr;
	}
	

	return Dpc;

}

void EnumerateDPCs( _In_ PKDPC& Dpc ) 	{

	auto NextEntry = Dpc->DpcListEntry.Next;

	while ( NextEntry != nullptr ) 		{

		auto CurrentDpc = CONTAINING_RECORD( NextEntry , KDPC , DpcListEntry );

		TraceLoggingWrite( g_Provider ,
						   "DPC Enumeration" ,
						   TraceLoggingLevel( TRACE_LEVEL_INFORMATION ),
						   TraceLoggingPointer( CurrentDpc , "DPC" ) ,
						   TraceLoggingValue(( CurrentDpc->Type == 0x13 ) ? "DPC_NORMAL" : "DPC_THREADED" , "Type" ),
						   TraceLoggingUInt32( CurrentDpc->Number , "Number" ) ,
						   TraceLoggingPointer( CurrentDpc->DeferredRoutine , "DeferredRoutine" ) ,
						   TraceLoggingPointer( CurrentDpc->DpcData , "DpcData" ),
						   TraceLoggingUInt8( CurrentDpc->Importance , "Importance" )
		);

		NextEntry = CurrentDpc->DpcListEntry.Next;
	}

	return;
}

