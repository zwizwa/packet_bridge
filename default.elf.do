# So emacs compile mode knows where we are.
if [ ! -z "REDO_VERBOSE_ENTER" ]; then 
    echo "redo: Entering directory '$(readlink -f .)'" >&2
fi
O="$2_main.o packet_bridge.o"
redo-ifchange $O
gcc -o $3 $O



# Note: this supports arbitrary applications.  Just link in the
# xxx_main.c file and run "redo xxx.elf"

