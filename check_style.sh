#!/bin/sh
# Requires some non-POSIX features found eg in GNU or FreeBSD's bsdgrep, namely
#   --include
#   -R
# Apart from that, this should work on any POSIX-compliant system

# This Script checks for some common styling issues. Ideally a commit should pass
# ./check_style.sh, but passing ./check_style.sh does not guarantee the abscence
# of styling issues

# Init
cd "$(git rev-parse --show-toplevel)"

flag=0
tab="$(printf '\t')"
## Useful (E)RE snippets
ere_par_kw='(if|for|while|switch)'
re_alnum='[a-zA-Z0-9]'

if [ -z "$NOCOLOUR" ] ; then
    cstart='\e[1;31m'
    cend='\e[0m'
else
    cstart=''
    cend=''
fi

# Helper functions
print_err() {
    flag=1
    echo "$cstart$@$cend" >&2
}

found() {
    if [ "$?" -eq 0 ] ; then
        print_err "$*"
    fi
}

grepc() {
    grep --include='*.c' --include='*.h' --exclude='wyhash.h' -Rn "$@"
}

#########################################################################
# Warn about unstaged changes
if LC_ALL=C git status | grep -q 'Changes not staged for commit' ; then
	print_err "There are unstaged changes !"
fi
########################################################################

# Print error and exit on unset variables (NOCOLOUR might be unset)
set -u


# Whitespace Type
grepc "$tab" .
found "Tabs instead of spaces!"

# Superfluous Whitespace
grepc -E '[ '"$tab"']+$' .
found "Lines with trailing whitespace!"

# Keywords and Whitespace
grepc -E "$ere_par_kw"'\(|'"$re_alnum"'\{|\}'"$re_alnum" .
found "Keyword etc not delimited by whitespace from parentheses or brackets!"

grepc -E '( |^)'"$ere_par_kw"'  +\(|'"$re_alnum"'  +\{|\}  +'"$re_alnum" .
found "Keyword etc delimited with too much whitespace from '(','{' or '}'!"

# Parentheses, Brackets and Whitespace
grepc '){' .
found "Parenthesis and bracket not delimited by whitespace!"

# sizeof and whitespace
grepc -E 'sizeof +\(' .
found "'sizeof' delimited by whitespace!"

# Trigraphs are evil
grepc -E '\?\?([=/()!<>'\'']|-)'
found "Trigraphs!"

LC_CTYPE=C grepc '[^[:cntrl:][:print:]±¡]'
found "Unknown non-ASCII characters!"

# Finish
if [ "$flag" -ne 0 ] ; then
    print_err "[Not-OK]: see messages above"
else
    echo "[OK]: No style violations found." >&2
fi

exit "$flag"
