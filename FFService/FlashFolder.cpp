/* This file is part of FlashFolder. 
 * Copyright (C) 2007 zett42 ( zett42 at users.sourceforge.net ) 
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */
/* \file FlashFolder service.
  The service is used to wait for session logon events and start an admin process
  in the new users session which in turn installs the global FlashFolder hook function
  contained in "fflib.dll". This way FlashFolder is available in admin processes,
  even if the logged-on user has restricted rights.
*/
#include "stdafx.h"
#include "resource.h"
#include "logfile.h"
#include "../fflib/fflib_exports.h"

HINSTANCE g_hInstance = NULL;

const TCHAR INTERNAL_APPNAME[] = L"FlashFolder"; 

//---------------------------------------------------------------------------
// global state

SERVICE_STATUS          g_myServiceStatus = { SERVICE_WIN32_OWN_PROCESS }; 
SERVICE_STATUS_HANDLE   g_myServiceStatusHandle = NULL; 

CHandle g_serviceTerminateEvent;
CHandle g_hookTerminateEvent;

LogFile g_logfile;

//---------------------------------------------------------------------------

void Log( LPCTSTR str ) 
{ 
	g_logfile.Write( str );
}

void Log( LPCTSTR str, DWORD status ) 
{ 
	TCHAR msg[ 1024 ]; swprintf_s( msg, L"%s (%d)", str, status );
	g_logfile.Write( msg );
}

//---------------------------------------------------------------------------
/// Notify the SCM about a changed service status.
/// Algorithm taken from the MSDN example "Writing a service main function". 

void SetMyServiceStatus( DWORD status, DWORD err = 0, DWORD waitHint = 0 )
{
	static DWORD s_checkPoint = 1;

	g_myServiceStatus.dwWin32ExitCode = err; 
	g_myServiceStatus.dwCurrentState  = status; 
	g_myServiceStatus.dwWaitHint      = waitHint; 
	
	if( status == SERVICE_START_PENDING )
		g_myServiceStatus.dwControlsAccepted = 0;
	else
		g_myServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | 
											   SERVICE_ACCEPT_SESSIONCHANGE;
											   
	if( status == SERVICE_RUNNING || status == SERVICE_STOPPED )
		g_myServiceStatus.dwCheckPoint = 0;
	else 
		g_myServiceStatus.dwCheckPoint = s_checkPoint++;											   

	if( ! ::SetServiceStatus( g_myServiceStatusHandle, &g_myServiceStatus ) )
	{ 
		DWORD err = ::GetLastError(); 
		Log( L"SetServiceStatus() error", err ); 
	} 
}

//---------------------------------------------------------------------------
/// Notify the SCM about unchanged service status.

void SetMyServiceStatusUnchanged()
{
	if( ! ::SetServiceStatus( g_myServiceStatusHandle, &g_myServiceStatus ) )
	{ 
		DWORD err = ::GetLastError(); 
		Log( L"SetServiceStatus() error", err ); 
	} 
}

//---------------------------------------------------------------------------
/// Start an instance of the current process in a different session
/// but with the same access rights as the current process.
/// This is to support "fast user switching" available beginning with XP.

bool StartHookProcessInSession( DWORD dwSessionId )
{
	Log( L"Starting hook process in session", dwSessionId );

	CHandle hProcessToken;
	if( ! ::OpenProcessToken( ::GetCurrentProcess(), TOKEN_ALL_ACCESS, &hProcessToken.m_h ) )
	{
		Log( L"OpenProcessToken() failed", ::GetLastError() );
		return false;
	}	
	
	CHandle hDupToken;
	if( ! ::DuplicateTokenEx( hProcessToken, 0, NULL, SecurityImpersonation, 
	                          TokenPrimary, &hDupToken.m_h ) )
	{
		Log( L"DuplicateToken() failed", ::GetLastError() );
		return false;
	}					

	if( ! ::SetTokenInformation( hDupToken, TokenSessionId, &dwSessionId, sizeof(DWORD) ) )
	{
		Log( L"SetTokenInformation() failed", ::GetLastError() );
		return false;
	}
	
	STARTUPINFO si = { sizeof(si) };
	si.lpDesktop = L"WinSta0\\Default";  // The desktop of the interactive WinStation.
	PROCESS_INFORMATION pi = { 0 };
	WCHAR cmd[ 1024 ] = L"\"";
	WCHAR exePath[ 1024 ] = L"";
	::GetModuleFileName( NULL, exePath, _countof( exePath ) );
	wcscat_s( cmd, exePath );
	wcscat_s( cmd, L"\" /sethook" );
	 
	// If the FlashFolder hook process is already running in another session, 
	// CreateProcessAsUser() returns ERROR_PIPE_NOT_CONNECTED if we call it immediately
	// after we received SERVICE_CONTROL_SESSIONCHANGE notification.
	// I could not find no another way around this then to call CreateProcessAsUser() 
	// repeatedly in this case. 
	DWORD time0 = ::GetTickCount();
	do	
	{	 
		if( ::CreateProcessAsUser( hDupToken, NULL, cmd, NULL, NULL, FALSE, 0,
					  NULL, NULL, &si, &pi ) )
		{
			Log( L"Hook process created" );
			::CloseHandle( pi.hThread );
			::CloseHandle( pi.hProcess );
			return true;
		}					  
					
		DWORD err = ::GetLastError();
		Log( L"CreateProcessAsUser failed", err );
	
		if( err != ERROR_PIPE_NOT_CONNECTED )
			return false;
			
		::Sleep( 2000 );
		Log( L"Retry..." );
	}
	while( ::GetTickCount() - time0 < 30000 );

	Log( L"Starting hook process failed because of timeout" );
	
	return false;
}

//---------------------------------------------------------------------------
/// Handler that receives events from the service manager.

DWORD WINAPI MyServiceCtrlHandler( DWORD control, DWORD eventType, 
	LPVOID pEventData, LPVOID pContext ) 
{ 
	switch( control ) 
	{ 
		case SERVICE_CONTROL_STOP: 
		{
			Log( L"SERVICE_CONTROL_STOP event received" );
			
			SetMyServiceStatus( SERVICE_STOP_PENDING, 0, 5000 );
			::PulseEvent( g_serviceTerminateEvent );

			return NO_ERROR; 
		}

		case SERVICE_CONTROL_SHUTDOWN:
		{
			Log( L"SERVICE_CONTROL_SHUTDOWN event received" );
			
			SetMyServiceStatus( SERVICE_STOP_PENDING, 0, 5000 );
			::PulseEvent( g_serviceTerminateEvent );
			
			return NO_ERROR;
		}
		
		case SERVICE_CONTROL_SESSIONCHANGE:
		{
			SetMyServiceStatusUnchanged();

			if( eventType == WTS_SESSION_LOGON )
			{
				WTSSESSION_NOTIFICATION* pSession = (WTSSESSION_NOTIFICATION*) pEventData;

				Log( L"WTS_SESSION_LOGON event received", pSession->dwSessionId );				
				
				// Hint to self: wait for event "Global\\TermSrvReadyEvent" if you need
				// terminal services functions like WtsQuerySessionInformation() here. 
				
				StartHookProcessInSession( pSession->dwSessionId );
                  								
				return NO_ERROR;			
			}
		
			return ERROR_CALL_NOT_IMPLEMENTED;
		}

		case SERVICE_CONTROL_INTERROGATE:
		{
			SetMyServiceStatusUnchanged();
			
			return NO_ERROR;
		}
	} 
	
	Log( L"Unrecognized control received", control ); 

	SetMyServiceStatusUnchanged();

	return ERROR_CALL_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
/// Start one hook process per user session (if any).

bool StartHookInCurrentSessions()
{
	Log( L"StartHookInCurrentSessions()" );

	PWTS_SESSION_INFO pSessions = NULL;
	DWORD sessionCount = 0;

	// This is expected to fail if there are no user sessions available at the time.
	if( ! ::WTSEnumerateSessions( WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessions, &sessionCount ) )
	{
		Log( L"WTSEnumerateSessions() error", ::GetLastError() );
		return false;
	}
	
	for( int i = 0; i < sessionCount; ++i )
	{
		WCHAR s[ 256 ] = L"";
		swprintf_s( s, L"Session ID: %d, state: %d, WinStation: %s",
			pSessions[ i ].SessionId, pSessions[ i ].State, pSessions[ i ].pWinStationName );
		Log( s );
		
		if( pSessions[ i ].State != WTSActive && 
		    pSessions[ i ].State != WTSConnected && 
		    pSessions[ i ].State != WTSDisconnected &&
		    pSessions[ i ].State != WTSIdle )
		{
			Log( L"\tSkipping session because of its state" );
			continue;	
		}
		
		StartHookProcessInSession( pSessions[ i ].SessionId );

		// Notify the SCM so it doesn't think the service is hung.
		SetMyServiceStatusUnchanged();
	}
	
	::WTSFreeMemory( pSessions );
	
	return true;
}

//---------------------------------------------------------------------------

bool InitService()
{
	// Create event for service termination 
	g_serviceTerminateEvent.Attach( ::CreateEvent( NULL, FALSE, FALSE, NULL ) );
	
	// Create event for hook termination
	g_hookTerminateEvent.Attach( 
		::CreateEvent( NULL, TRUE, FALSE, L"Global\\FFHookTerminateEvent_du38hndkj4" ) );

	if( ! g_hookTerminateEvent )
	{
		Log( L"Failed to create hook termination event.", ::GetLastError() );
		return false;
	}
		
	if( ::GetLastError() == ERROR_ALREADY_EXISTS )
	{
		// If the event exists, it could have been created by another process
		// with lower privileges, so we cannot trust it.		
	
		Log( L"Cannot trust existing hook termination event." );
		return false;
	}
	
	// Start the hook so it is available immediately after installation,
	// if the installation is done in an active user session (not for group 
	// policy installation which is done "per-machine" without user context).
	StartHookInCurrentSessions();
	
	return true;
}

//---------------------------------------------------------------------------

void FinalizeService()
{
	// Terminate hook processes in all sessions.
	Log( L"Setting hook termination event" );
	if( ! ::SetEvent( g_hookTerminateEvent ) )
		Log( L"Failed to set hook termination event", ::GetLastError() );
}

//---------------------------------------------------------------------------

void WINAPI ServiceMain( DWORD argc, LPTSTR *argv ) 
{ 
    Log( L"ServiceMain()" ); 

	g_myServiceStatusHandle = ::RegisterServiceCtrlHandlerEx( 
        INTERNAL_APPNAME, MyServiceCtrlHandler, NULL ); 
 
    if( g_myServiceStatusHandle == (SERVICE_STATUS_HANDLE)0 ) 
    { 
		Log( L"RegisterServiceCtrlHandler() failed", ::GetLastError() ); 
        return;
    } 
    
    SetMyServiceStatus( SERVICE_START_PENDING, 0, 30000 );
 
	if( InitService() )
	{
		// Initialization complete - report running status. 
		SetMyServiceStatus( SERVICE_RUNNING );
		
		Log( L"Service running, waiting for events..." );

		::WaitForSingleObject( g_serviceTerminateEvent, INFINITE );
		::CloseHandle( g_serviceTerminateEvent );

		FinalizeService();
	}
	
	Log( L"Service stopped." );
	SetMyServiceStatus( SERVICE_STOPPED );

    return; 
} 

//---------------------------------------------------------------------------

int StartService()
{
	g_logfile.Open( L"_ffservice_log.txt" );
	Log( L"Task: Starting service" );

	SERVICE_TABLE_ENTRY dispatchTable[] = 
	{ 
		{ (LPWSTR) INTERNAL_APPNAME, ServiceMain }, 
		{ NULL, NULL } 
	}; 

	if( ! ::StartServiceCtrlDispatcher( dispatchTable ) ) 
	{ 
		DWORD err = ::GetLastError();
		if( err == ERROR_SERVICE_ALREADY_RUNNING )	
		{
			Log( L"Service is already running." );
			return 0;
		}

		Log( L"StartServiceCtrlDispatcher() error, exiting", err );
		
		return 1;
	} 
	return 0;
}

//---------------------------------------------------------------------------

LRESULT CALLBACK HookWndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
	if( msg == WM_DESTROY )
	{
		Log( L"WM_DESTROY received - exiting" );
		::PostQuitMessage( 0 );
	}	
	else if( msg == WM_ENDSESSION && wParam == TRUE )
	{
		Log( L"WM_ENDSESSION received - exiting" );
		::PostQuitMessage( 0 );	
	}

	return ::DefWindowProc( hWnd, msg, wParam, lParam );
}

//---------------------------------------------------------------------------

void OpenHookLogFile()
{
	DWORD sessionId = -1;
	::ProcessIdToSessionId( ::GetCurrentProcessId(), &sessionId );

	WCHAR logName[ 100 ]; swprintf_s( logName, L"_ffhook_log%d.txt", sessionId );
	g_logfile.Open( logName );
}

//---------------------------------------------------------------------------

int SetHook()
{
	OpenHookLogFile();
	Log( L"Task: Starting hook" );
			
	// Open/create event for hook termination
	CHandle hookTerminateEvent( 
		::CreateEvent( NULL, TRUE, FALSE, L"Global\\FFHookTerminateEvent_du38hndkj4" ) );
	if( ! hookTerminateEvent )
	{
		Log( L"Failed to create hook termination event.", ::GetLastError() );
		return 1;
	}	

	Log( L"Installing hook..." );
	if( ! InstallHook() )
	{
		DWORD err = ::GetLastError();
		Log( L"InstallHook() failed, exiting", err );
		return 1;
	}	
	
	
	//--- Run message loop to keep hook alive.
	
	Log( L"Running message loop..." );
	
	WNDCLASS wc = { 0, HookWndProc, 0, 0, g_hInstance, 0, 0, 0, 0, L"FlashFolder.jks2hd4fenjcnd3" };
	::RegisterClass( &wc );
	HWND hwnd = ::CreateWindow( wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, g_hInstance, 0 );
	if( ! hwnd )
	{
		Log( L"Failed to create message window", ::GetLastError() );
		return 1;
	}
	
	for(;;)
	{
		DWORD waitRes =	::MsgWaitForMultipleObjects( 1, &hookTerminateEvent.m_h, FALSE, INFINITE, QS_ALLINPUT );

		// TODO: Check if FlashFolder toolbar window(s) exists. If yes, we must not exit
		//       this process since it would crash any program hosting the FlashFolder toolbar.
		
		if( waitRes == WAIT_OBJECT_0 )
		{
			Log( L"Termination event received" );
			break;
		}
		else if( waitRes == WAIT_OBJECT_0 + 1 )
		{
			MSG msg;		
			if( ::GetMessage( &msg, NULL, 0, 0 ) <= 0 )
			{	
				Log( L"GetMessage() return value <= 0." );
				break;
			}
		
			Log( L"Message received", msg.message );
		
			::TranslateMessage( &msg );
			::DispatchMessage( &msg );	
		}
		else
		{
			Log( L"Unexpected return value of MsgWaitForMultipleObjects()", waitRes );
			break;
		}
	}
	 
	::UnregisterClass( wc.lpszClassName, g_hInstance );	

	Log( L"Hook process exits" );

	return 0;
}

//---------------------------------------------------------------------------
/// Remove the FlashFolder hook for all sessions.

int RemoveHook()
{
	OpenHookLogFile();
	Log( L"Task: Removing hook" );
	
	// Open/create event for hook termination
	CHandle hookTerminateEvent( 
		::OpenEvent( EVENT_MODIFY_STATE, FALSE, L"Global\\FFHookTerminateEvent_du38hndkj4" ) );
	if( ! hookTerminateEvent )
	{
		Log( L"Failed to open hook termination event.", ::GetLastError() );
		return 1;
	}	
	if( ! ::SetEvent( hookTerminateEvent ) )
	{
		Log( L"Failed to set hook termination event", ::GetLastError() );
		return 1;
	}

	Log( L"Termination event set" );
	
	return 0;
}

//---------------------------------------------------------------------------

void ShowHelp()
{
	::MessageBox( NULL,
		L"FlashFolder command line syntax:\n\n"
		L"FlashFolder.exe <Parameter>\n\n"
		L"<Parameter> can be one of:\n"
		L"/startservice Starts the FlashFolder service and activates the hook for any active user sessions.\n"
		L"/sethook Activates the FlashFolder hook.\n"
		L"/unhook Terminates the FlashFolder hook.\n",
		L"FlashFolder commandline help",
		MB_OK );
}

//---------------------------------------------------------------------------

int WINAPI _tWinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow )         
{ 
	g_hInstance = hInstance;

	if( wcsstr( lpCmdLine, L"/startservice" ) )
	{	
		return StartService();	
	}
	else if( wcsstr( lpCmdLine, L"/sethook" ) )
	{
		return SetHook();
	}
	else if( wcsstr( lpCmdLine, L"/unhook" ) )
	{
		return RemoveHook();
	}
	else
	{
		ShowHelp();
		return 1;
	}

	return 0;
} 
