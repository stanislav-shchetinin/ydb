DLL(nfs_fault)

SRCS(
    nfs_fault.c
)

CFLAGS(
    -fPIC
)

EXTRALIBS(
    -ldl
)

END()
