on discardZip(zipPath)
    -- The caller checked that this is its download in a private temporary directory.
    try
        do shell script "/bin/rm -f " & quoted form of zipPath & " && /bin/rmdir " & quoted form of (do shell script "/usr/bin/dirname " & quoted form of zipPath)
    end try
end discardZip

on run argv
    set shellBody to item 1 of argv
    set oldPid to item 2 of argv
    set zipPath to item 3 of argv
    set targetApp to item 7 of argv
    set adminMode to item 8 of argv is "admin"
    set options to ""
    if (count argv) is 11 then set options to " --args --profile-dir " & quoted form of (item 11 of argv)
    set reopen to "/usr/bin/open -n " & quoted form of targetApp & options
    -- Let the app close its vault and release all profile and IPC locks.
    repeat 600 times
        try
            do shell script "/bin/kill -0 " & quoted form of oldPid
        on error
            exit repeat
        end try
        delay 0.1
    end repeat
    try
        do shell script "/bin/kill -0 " & quoted form of oldPid
        my discardZip(zipPath)
        display dialog "KEEFI가 아직 실행 중이라 업데이트를 중지했습니다." buttons {"확인"} default button 1 giving up after 30
        return
    end try
    set cmd to "/usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin /bin/sh -c " & quoted form of shellBody & " keefi-update"
    repeat with i from 3 to 9
        set cmd to cmd & " " & quoted form of (item i of argv)
    end repeat
    if (count argv) is 11 then set cmd to cmd & " " & quoted form of (item 11 of argv)
    try
        if adminMode then
            do shell script cmd with administrator privileges
        else
            do shell script cmd
        end if
    on error details number reason
        try
            do shell script reopen
        end try
        if reason is not -128 then display dialog "업데이트를 완료하지 못했습니다." & return & details buttons {"확인"} default button 1 giving up after 30
    end try
    my discardZip(zipPath)
end run
