# So emacs compile mode knows where we are.
if [ ! -z "REDO_VERBOSE_ENTER" ]; then 
    echo "redo: Entering directory '$(readlink -f .)'" >&2
fi
redo-ifchange $2.c
gcc -Wall -Werror -o $3 $2.c
