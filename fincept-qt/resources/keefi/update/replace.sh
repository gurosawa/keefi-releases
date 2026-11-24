# Fixed transaction embedded in the running app. All variable inputs are argv.
set -eu
umask 077
archive=$1 hash=$2 version=$3 build=$4 target=$5 privilege=$6 user_id=$7
profile=${8-}
parent=${target%/*}
fail() { echo "$1" >&2; exit 1; }
[ "${target##*/}" = KEEFI.app ] && [ ! -L "$target" ] && [ -d "$target" ] || fail "KEEFI.app 설치 경로를 확인해 주세요."
if [ "$privilege" = admin ]; then
    [ "$target" = /Applications/KEEFI.app ] && [ "$(/usr/bin/id -u)" = 0 ] && [ "$user_id" -gt 0 ] || fail "허용되지 않은 설치 경로입니다."
else
    [ "$(/usr/bin/id -u)" = "$user_id" ] || fail "설치 사용자를 확인할 수 없습니다."
fi
plist() { /usr/libexec/PlistBuddy -c "Print :$2" "$1/Contents/Info.plist"; }
id=xyz.beamgrove.keefi.local
[ "$(plist "$target" CFBundleIdentifier)" = "$id" ] || fail "설치된 앱 식별자가 다릅니다."
lock="$parent/.keefi-update.lock"
/usr/bin/shlock -p $$ -f "$lock" || fail "다른 업데이트가 진행 중입니다."
stage=""
keep_backup=0
cleanup() {
    if [ -n "$stage" ] && [ "$keep_backup" = 0 ]; then /bin/rm -rf "$stage"; fi
    /bin/rm -f "$lock"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
old=$(plist "$target" CFBundleVersion)
[ "$build" -gt "$old" ] || fail "설치된 버전과 업데이트 순서를 확인해 주세요."
stage=$(/usr/bin/mktemp -d "$parent/.keefi-restart.XXXXXXXX")
# This private, same-volume copy prevents a changed download from being used
# after authentication or while the administrator consent dialog was open.
/bin/cp "$archive" "$stage/payload.zip"
actual=$(/usr/bin/shasum -a 256 "$stage/payload.zip")
[ "${actual%% *}" = "$hash" ] || fail "다운로드한 파일이 변경되어 설치를 중지했습니다."
/usr/bin/ditto -x -k "$stage/payload.zip" "$stage"
next="$stage/KEEFI.app"
[ ! -L "$next" ] && [ -d "$next" ] && [ -x "$next/Contents/MacOS/KEEFI" ] || fail "앱 번들을 찾을 수 없습니다."
[ "$(plist "$next" CFBundleIdentifier)" = "$id" ] &&
[ "$(plist "$next" CFBundleExecutable)" = KEEFI ] &&
[ "$(plist "$next" CFBundleVersion)" = "$build" ] &&
[ "$(plist "$next" CFBundleShortVersionString)" = "$version" ] || fail "업데이트 번들의 버전 또는 식별자가 다릅니다."
/usr/bin/codesign --verify --deep --strict "$next" || fail "새 앱의 코드 무결성을 확인하지 못했습니다."
swap() {
    /usr/bin/osascript -l JavaScript -e 'function run(a) {
        ObjC.bindFunction("renamex_np", ["int", ["char *", "char *", "unsigned int"]]);
        if ($.renamex_np(a[0], a[1], 2) !== 0) throw Error("KEEFI.app 교체 실패");
    }' "$next" "$target"
}
# Darwin RENAME_SWAP exchanges whole directory trees atomically.
swap
keep_backup=1
set -- /usr/bin/open -n "$target"
if [ -n "$profile" ]; then set -- "$@" --args --profile-dir "$profile"; fi
if [ "$privilege" = admin ]; then
    set -- /bin/launchctl asuser "$user_id" /usr/bin/sudo -H -u "#$user_id" -- "$@"
fi
if ! "$@"; then
    if swap; then keep_backup=0; fi
    fail "새 앱을 열지 못했습니다."
fi
keep_backup=0
