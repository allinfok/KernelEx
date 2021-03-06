/*
 *  KernelEx
 *
 *  Copyright (C) 2010, Tihiy
 *  Copyright (C) 2013, Ley0k
 *
 *  This file is part of KernelEx source code.
 *
 *  KernelEx is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published
 *  by the Free Software Foundation; version 2 of the License.
 *
 *  KernelEx is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/* various thuni routines here */

#include <windows.h>
#include <kexcoresdk.h>
#include "thuni_layer.h"
#include "desktop.h"

LPCRITICAL_SECTION pWin16Mutex;
PUSERDGROUP gSharedInfo;
DWORD UserHeap;
HMODULE g_hUser32;
HMODULE g_hUser16;
HMODULE g_hKernel16;
DWORD pLDT; 
WORD selLDT;
DWORD pSelTables;
DWORD pBurgerMaster;
DWORD CallProc32W;
DWORD EnterWin16Lock16;
DWORD LeaveWin16Lock16;

static DWORD GFSR_Address;
static DWORD _UserSeeUserDo;

static BOOL FASTCALL InitUSERHooks(void);

BOOL InitUniThunkLayerStuff()
{
	g_hUser16 = (HMODULE)LoadLibrary16("user");
	g_hKernel16 = (HMODULE)LoadLibrary16("KRNL386.EXE");
	DWORD* ldt;

	if((DWORD)g_hKernel16 < 32)
		g_hKernel16 = (HMODULE)LoadLibrary16("kernel");

	if((DWORD)g_hUser16 < 32 || (DWORD)g_hKernel16 < 32)
		return FALSE;

	/* Get LDT table start */
	ldt = (DWORD*)((DWORD)MapSL + 33);
	pLDT = *(DWORD*)*ldt;

	/* Get LDT segment */
	ldt = (DWORD*)((DWORD)MapSL + 20);
	selLDT = *(WORD*)*ldt;

	/* Get global arenas offset */
	ldt = (DWORD*)((DWORD)K32WOWGetVDMPointerFix + 48);
	pSelTables = *(DWORD*)*ldt;

	/* Get burger master */
	ldt = (DWORD*)((DWORD)K32WOWGetVDMPointerFix + 64);
	pBurgerMaster = *(DWORD*)*ldt;

	EnterWin16Lock16 = GetProcAddress16(g_hKernel16, (LPSTR)480);
	LeaveWin16Lock16 = GetProcAddress16(g_hKernel16, (LPSTR)481);
	CallProc32W = GetProcAddress16(g_hKernel16, "CallProc32W");

	_GetpWin16Lock( &pWin16Mutex );
	gSharedInfo = (PUSERDGROUP)MapSL((DWORD)g_hUser16 << 16);
	g_hUser32 = GetModuleHandleA("user32");

	if(gSharedInfo != NULL)
		UserHeap = *(DWORD*)((DWORD)gSharedInfo + 0x1007C);

	GFSR_Address = GetProcAddress16(g_hUser16, "GetFreeSystemResources");

	if(GFSR_Address == 0)
	{
		TRACE_OUT("Can't find GetFreeSystemResources in USER.EXE !\n");
		return 0;
	}

	_UserSeeUserDo = GetProcAddress16(g_hUser16, "USERSEEUSERDO");

	if(_UserSeeUserDo == 0)
	{
		TRACE_OUT("Can't find UserSeeUserDo in USER.EXE !\n");
		return 0;
	}

	if(!InitUSERHooks())
	{
		TRACE_OUT("Failed to hook USER.EXE functions !\n");
		return 0;
	}

	TRACE("ThunkLayer initialized: gSharedInfo = 0x%X, g_hUser16 = 0x%X, g_hKernel16 = 0x%X, g_hUser32 = 0x%X\n", gSharedInfo, g_hUser16, g_hKernel16, g_hUser32);
	return (gSharedInfo && g_hUser32);
}

void GrabWin16Lock()
{
	if(pWin16Mutex == NULL)
	{
		kexGrabWin16Lock();
		return;
	}

	_EnterSysLevel(pWin16Mutex);
}

void ReleaseWin16Lock()
{
	if(pWin16Mutex == NULL)
	{
		kexReleaseWin16Lock();
		return;
	}

	_LeaveSysLevel(pWin16Mutex);
}

DWORD GetWindowProcessId( HWND hwnd )
{
	DWORD procID = 0;
	GetWindowThreadProcessId( hwnd, &procID );
	return procID;
}

__declspec(naked)
LRESULT WINAPI CallWindowProc_stdcall( WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
__asm {
	push ebp
	mov ebp, esp
	push edi
	push esi
	push ebx
	sub esp, 12
	push [ebp+24]
	push [ebp+20]
	push [ebp+16]
	push [ebp+12]
	mov eax, [ebp+8]
	call eax
	lea esp, [ebp-12]
	pop ebx
	pop esi
	pop edi
	leave
	ret 20
	}	
}

int GetCPFromLocale(LCID Locale)
{
	int cp;	
	Locale = LOWORD(Locale);
	if (GetLocaleInfoA(Locale,LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,(LPSTR)&cp,sizeof(int)))
		return cp;
	else
		return CP_ACP;
}

UINT GetCurrentKeyboardCP()
{
	return GetCPFromLocale((LCID)GetKeyboardLayout(0));
}

#pragma warning (disable:4035)
static inline
WORD GetCurrentThreadhQueue() 
{
	__asm mov ax, fs:[28h]
}
#pragma warning (default:4035)

PMSGQUEUE GetCurrentThreadQueue()
{
	return (PMSGQUEUE)MapSL( GetCurrentThreadhQueue() << 16 );
}

//per-thread keyboard state
static PQUEUEKEYBUFFER GetCurrentThreadKeyBuffer()
{	
	PMSGQUEUE msgq = GetCurrentThreadQueue();
	if (!msgq) return NULL;
	PPERQUEUEDATA queuedata = (PPERQUEUEDATA)REBASEUSER(msgq->npPerQueue);
	if (!queuedata) return NULL;
	return (PQUEUEKEYBUFFER)MapSL(queuedata->keysbuffer);	
}

void UpdateLRKeyState(LPMSG msg)
{
	switch(msg->message) {
	case WM_KEYDOWN:
	case WM_KEYUP:
	case WM_SYSKEYDOWN:
	case WM_SYSKEYUP:
		if (msg->wParam == VK_SHIFT)
		{
			PQUEUEKEYBUFFER keybuffer = GetCurrentThreadKeyBuffer();
			if (keybuffer)
			{
				BYTE scancode = LOBYTE(HIWORD(msg->lParam));
				if ( scancode == MapVirtualKey(VK_SHIFT,0) ) //left shift
					keybuffer->keystate[VK_LSHIFT] = keybuffer->keystate[VK_SHIFT];
				else
					keybuffer->keystate[VK_RSHIFT] = keybuffer->keystate[VK_SHIFT];
			}
		}
		else if (msg->wParam == VK_CONTROL)
		{
			PQUEUEKEYBUFFER keybuffer = GetCurrentThreadKeyBuffer();
			if (keybuffer)
			{
				if ( msg->lParam & 0x1000000 ) //extended bit -> right
					keybuffer->keystate[VK_RCONTROL] = keybuffer->keystate[VK_CONTROL];
				else
					keybuffer->keystate[VK_LCONTROL] = keybuffer->keystate[VK_CONTROL];					
			}
		}
		else if (msg->wParam == VK_MENU)
		{
			PQUEUEKEYBUFFER keybuffer = GetCurrentThreadKeyBuffer();
			if (keybuffer)
			{
				if ( msg->lParam & 0x1000000 ) //extended bit -> right
					keybuffer->keystate[VK_RMENU] = keybuffer->keystate[VK_MENU];
				else
					keybuffer->keystate[VK_LMENU] = keybuffer->keystate[VK_MENU];					
			}
		}
		break;
	}	
}

static BOOL FASTCALL InitUSERHooks(void)
{
	return TRUE;
}

__declspec(naked)
UINT GetFreeSystemResources(UINT uFlags)
{
__asm {

		push	ebp
		mov		ebp, esp
		sub		esp, 40h

		mov		edx, [GFSR_Address]
		xor		eax, eax

		push	word ptr [uFlags]
		call	ds:QT_Thunk

		movzx	eax, ax

		leave
		retn
	}
}

__declspec(naked)
UINT UserSeeUserDo(WORD wAction, WORD wParam1, WORD wParam2, WORD wParam3)
{
__asm {

		push	ebp
		mov		ebp, esp
		sub		esp, 40h

		mov		edx, [_UserSeeUserDo]
		xor		eax, eax

		push	word ptr [wAction]
		push	word ptr [wParam1]
		push	word ptr [wParam2]
		push	word ptr [wParam3]
		call	ds:QT_Thunk

		movzx	eax, ax
		and		edx, 0FFFFh
		add		eax, edx

		leave
		retn	8
	}
}
