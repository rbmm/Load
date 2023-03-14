
.code

; long __cdecl NT::retFromMapViewOfSection(long)
extern ?retFromMapViewOfSection@NT@@YAJJ@Z : PROC

; long __cdecl NT::aretFromMapViewOfSection(void)
?aretFromMapViewOfSection@NT@@YAJXZ proc
	mov ecx,eax
	call ?retFromMapViewOfSection@NT@@YAJJ@Z
?aretFromMapViewOfSection@NT@@YAJXZ endp

END