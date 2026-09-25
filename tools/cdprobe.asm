; MSCDEX probe (doc 17 §5.3): drive count (1500h), the drive check (150Bh) and the
; IOCTL INPUT subfunctions 06/0A/0B/0E/0C/09 through 1510h, each with its status
; word and the first 16 bytes of the control block. `nasm -f bin -o CDPROBE.COM
; tools/cdprobe.asm`; run it from any DOS with a CD driver, redirect to a file.
        org 100h
        mov ax,cs
        mov [rq+16],ax
        mov ax,1500h
        xor bx,bx
        int 2fh
        mov [ndrv],bx
        mov [first],cx
        mov dx,m1500
        call puts
        mov ax,[ndrv]
        call phex16
        mov dx,mfirst
        call puts
        mov ax,[first]
        call phex16
        call crlf
        mov ax,150bh
        mov cx,[first]
        int 2fh
        mov dx,m150b
        call puts
        call phex16
        mov ax,bx
        call phex16
        call crlf
        ; IOCTL input subfunctions
        mov al,6
        call ioctl
        mov al,0ah
        call ioctl
        mov byte [cb+1],1
        mov al,0bh
        call ioctl
        mov byte [cb+1],2
        mov al,0bh
        call ioctl
        mov byte [cb+1],47
        mov al,0bh
        call ioctl
        mov al,0eh
        call ioctl
        mov al,0ch
        call ioctl
        mov al,9
        call ioctl
        mov ax,4c00h
        int 21h
ioctl:  ; al = subfunction; clears the control block, sends it, prints status + 16 bytes
        mov [cb],al
        mov di,cb+2
        mov cx,30
        push ax
        xor al,al
        rep stosb
        pop ax
        mov word [rq+3],0
        mov ax,1510h
        mov cx,[first]
        push cs
        pop es
        mov bx,rq
        int 2fh
        mov dx,mioctl
        call puts
        mov al,[cb]
        call phex8
        mov dx,mstat
        call puts
        mov ax,[rq+3]
        call phex16
        mov dx,mdata
        call puts
        mov si,cb
        mov cx,16
.l:     lodsb
        call phex8
        mov dl,' '
        mov ah,2
        int 21h
        loop .l
        call crlf
        ret
phex16: push ax
        mov al,ah
        call phex8
        pop ax
phex8:  push ax
        shr al,4
        call nib
        pop ax
        and al,0fh
nib:    add al,'0'
        cmp al,'9'
        jbe .p
        add al,7
.p:     mov dl,al
        mov ah,2
        int 21h
        ret
crlf:   mov dx,mcrlf
puts:   mov ah,9
        int 21h
        ret
m1500   db 'MSCDEX 1500 drives=$'
mfirst  db ' first=$'
m150b   db '150B ax,bx=$'
mioctl  db 'IOCTL fn $'
mstat   db ' status=$'
mdata   db ' data=$'
mcrlf   db 13,10,'$'
ndrv    dw 0
first   dw 0
        align 2
rq      db 26,0,3
        dw 0
        times 8 db 0
        db 0
        dw cb
        dw 0
        dw 32
        dw 0
        dd 0
cb      times 32 db 0
