# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# bash completion for driftwire(1): its commands, its options, files after
# `send` and folders after --save-to. Installed as
# /usr/share/bash-completion/completions/driftwire.

_driftwire() {
    local cur prev words cword split
    if declare -F _init_completion >/dev/null 2>&1; then
        _init_completion -s || return  # "--save-to=DIR": prev is --save-to
    else
        COMPREPLY=()
        cur="${COMP_WORDS[COMP_CWORD]}"
        prev="${COMP_WORDS[COMP_CWORD-1]}"
        words=("${COMP_WORDS[@]}")
        cword=$COMP_CWORD
        # Without bash-completion, readline splits "--save-to=DIR" at "=".
        if [[ "$prev" == "=" && $cword -ge 2 ]]; then
            prev="${COMP_WORDS[COMP_CWORD-2]}"
        elif [[ "$cur" == "=" ]]; then
            prev="${COMP_WORDS[COMP_CWORD-1]}"
            cur=""
        fi
    fi

    local common="--save-to --name --plain --no-color --verbose --help --version"

    case "$prev" in
        --save-to)
            compopt -o filenames 2>/dev/null
            COMPREPLY=($(compgen -d -- "$cur"))
            return
            ;;
        --name|--to)
            return  # a name: nothing to offer
            ;;
    esac

    # The command: the first word that is not an option (or an option's value).
    local i cmd=""
    for ((i = 1; i < cword; i++)); do
        case "${words[i]}" in
            --save-to|--name|--to)
                ((i++))
                [[ "${words[i]}" == "=" ]] && ((i++))
                ;;
            -*|=) ;;
            *) cmd="${words[i]}"; break ;;
        esac
    done

    case "$cmd" in
        "")
            if [[ "$cur" == -* ]]; then
                COMPREPLY=($(compgen -W "$common" -- "$cur"))
            else
                COMPREPLY=($(compgen -W "send receive code devices help" -- "$cur"))
            fi
            ;;
        send)
            if [[ "$cur" == -* ]]; then
                COMPREPLY=($(compgen -W "--to $common" -- "$cur"))
            else
                compopt -o filenames 2>/dev/null
                COMPREPLY=($(compgen -f -- "$cur"))
            fi
            ;;
        receive)
            COMPREPLY=($(compgen -W "--once $common" -- "$cur"))
            ;;
        code)
            # Never a code: it is pasted into /to-code, not typed here.
            COMPREPLY=($(compgen -W "$common" -- "$cur"))
            ;;
    esac
}
complete -F _driftwire driftwire
