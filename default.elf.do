# So emacs compile mode knows where we are.
if [ ! -z "REDO_VERBOSE_ENTER" ]; then 
    echo "redo: Entering directory '$(readlink -f .)'" >&2
fi
redo-ifchange $2.c
#CFLAGS="-std=c99 -Wall -Werror"
CFLAGS="-std=c99"
gcc $CFLAGS -o $3 $2.c
