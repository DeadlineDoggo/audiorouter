#!/bin/sh
# release-notes.sh - writes the release notes (markdown) to stdout.
#
#   [DIST=dist] sh .github/scripts/release-notes.sh TAG [PREV_TAG] [END_REF]
#
# Versioning: vX.Y.0 is a full release, vX.Y.Z with Z > 0 is a nightly build; a
# suffix (v0.9.0-beta) is kept in the names and ignored by this logic.
# A full release lists everything since the previous full release (nightlies in
# between included); a nightly lists the changes since the previous tag.
# The changes are the commit subjects grouped by conventional-commit type.
# With DIST set, the Downloads table shows the real file names found there.
set -eu

TAG=$1
END=${3:-$TAG}
VER=${TAG#v}
BASE=${VER%%-*}
PATCH=${BASE##*.}
REPO=${GITHUB_REPOSITORY:-DeadlineDoggo/audiorouter}

if [ "$PATCH" = 0 ]; then KIND=full; else KIND=nightly; fi

PREV=${2:-}
if [ -z "$PREV" ]; then
    for t in $(git tag --list 'v[0-9]*' --sort=-creatordate); do   # version sort gets suffixes wrong
        [ "$t" = "$TAG" ] && continue
        git merge-base --is-ancestor "$t" "$END" 2>/dev/null || continue
        if [ "$KIND" = full ]; then
            b=${t#v}; b=${b%%-*}
            case $b in *.0) PREV=$t; break ;; esac
        else
            PREV=$t; break
        fi
    done
fi
RANGE=${PREV:+$PREV..}$END

# real asset name from DIST, or the glob when DIST is not set (local preview)
asset() {
    if [ -n "${DIST:-}" ]; then
        f=$(ls "$DIST"/$1 2>/dev/null | head -1 || true)
        [ -n "$f" ] && { basename "$f"; return; }
    fi
    echo "$1"
}

echo "SoundRoot routes application audio to output devices on KDE Plasma 6, and shares audio nodes between computers on your network."
echo
if [ "$KIND" = nightly ]; then
    echo "> **Nightly build.** Fresh from development, it may be rough around the edges."
    echo "> Full releases are the \`vX.Y.0\` ones."
    echo
fi
case $VER in *-*)
    echo "> **${VER#*-}.** Not finished: expect rough edges."
    echo
esac

cat <<E2
## Downloads

| System | File | Install |
|---|---|---|
| Debian 13, and systems with the same Plasma and Qt | \`$(asset '*.deb')\` | \`sudo apt install ./soundroot_*.deb\` |
| Fedora | \`$(asset '*.rpm')\` | \`sudo dnf install ./soundroot-*.rpm\` |
| Arch Linux | \`$(asset '*.pkg.tar.zst')\` | \`sudo pacman -U soundroot-*.pkg.tar.zst\` |
| any other distribution | \`$(asset 'soundroot-*.tar.gz')\` | unpack, then \`bash install-local.sh\` (builds from source) |

The packages install where Plasma looks for plugins, so nothing else has to be set up. Restart Plasma or log out and in, then add the widget. The packages are built on the newest Arch, Debian 13 and Fedora; on an older distribution use the source archive. Checksums are in \`SHA256SUMS\`.

## What's new

E2

# commit subjects since PREV (two messages typed into one subject are split;
# the version bump commits "chore: release vX.Y.Z" are left out)
LOG=$(git log --no-merges --pretty='%s' "$RANGE" |
      sed 's/ \(feat\|fix\|docs\|build\|ci\|chore\|refactor\|perf\|test\|style\)\([(:]\)/\n\1\2/g' |
      sed 's/[;,] *$//' |
      grep -vE '^chore(\([^)]*\))?: (release|bump version)' || true)

# One section. First the commits without a scope together with the scopes that
# occur once in this section ("- **Scope:** text"); then every scope that occurs
# two or more times as one bold bullet with its commits nested below it.
section() {   # title, regex of types
    out=$(printf '%s\n' "$LOG" | sed -n -E "s/^($2)(\\(([^)]*)\\))?!?: (.+)\$/\\3\\t\\4/p" |
          awk -F'\t' '
            function cap(s) { return toupper(substr(s, 1, 1)) substr(s, 2) }
            { n++; sc[n] = $1; tx[n] = $2
              if ($1 != "") { if (!($1 in cnt)) order[++no] = $1; cnt[$1]++ } }
            END {
              for (i = 1; i <= n; i++)
                if (sc[i] == "") printf "- %s\n", tx[i]
                else if (cnt[sc[i]] < 2) printf "- **%s:** %s\n", cap(sc[i]), tx[i]
              for (k = 1; k <= no; k++) {
                s = order[k]
                if (cnt[s] < 2) continue
                printf "- **%s**\n", cap(s)
                for (i = 1; i <= n; i++) if (sc[i] == s) printf "  - %s\n", tx[i]
              }
            }')
    [ -n "$out" ] && printf '### %s\n\n%s\n\n' "$1" "$out"
    return 0
}
section "New" "feat"
section "Fixes" "fix"
section "Performance" "perf"
section "Documentation" "docs"
section "Build and packaging" "build|ci"
section "Maintenance" "chore|refactor|test|style"
other=$(printf '%s\n' "$LOG" | grep -vE '^(feat|fix|perf|docs|build|ci|chore|refactor|test|style)(\(|:|!)' | grep -v '^$' | sed 's/^/- /' || true)
[ -n "$other" ] && printf '### Other\n\n%s\n\n' "$other"

if [ -n "$PREV" ]; then
    echo "**Full changelog:** https://github.com/$REPO/compare/$PREV...$TAG"
else
    echo "First release."
fi
