.686
.model flat

.code
; long __fastcall NT::retFromMapViewOfSection(long)
extern ?retFromMapViewOfSection@NT@@YIJJ@Z : PROC

; long __stdcall NT::aretFromMapViewOfSection(void)
?aretFromMapViewOfSection@NT@@YGJXZ proc
	mov ecx,eax
	call ?retFromMapViewOfSection@NT@@YIJJ@Z
?aretFromMapViewOfSection@NT@@YGJXZ endp

end
