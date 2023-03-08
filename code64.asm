
; long __cdecl retFromMapViewOfSection(long)
extern ?retFromMapViewOfSection@@YAJJ@Z : PROC

.code

; long __cdecl aretFromMapViewOfSection(void)
?aretFromMapViewOfSection@@YAJXZ proc
  mov ecx,eax
  call ?retFromMapViewOfSection@@YAJJ@Z
?aretFromMapViewOfSection@@YAJXZ endp

end
