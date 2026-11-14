<#
.SYNOPSIS
  King of the Hill end to end on one PC (docs/PROTOCOL.md 1.10.2): this checkout's server on a throwaway database and
  three of its launchers, A hosting a King of the Hill lobby, B joining it, C joining once both places are taken (it
  waits in line and watches), each with a real game and scripted attackers. A launcher readies whenever it is one of
  the two players and its game is free, and rates every match it watched once its game has shown the end (--rate).
  Afterwards a fourth launcher, D, opens the Create lobby window and the leaderboard. Prints the games' status
  lines, the server's King of the Hill lines (king, next up, ratings, Respect Points), what each launcher did, and the
  database's Respect Points.

  The launchers run offscreen and the games minimized, at below-normal priority, muted, with every controller off
  (a PC someone uses). -Snaps saves pictures of the lobby page as it changes, and D's of the Create lobby window and
  the leaderboard in both orders (build\koth-e2e\snaps). Everything the run started is closed at its end.

.EXAMPLE
  powershell -File tools\koth-e2e.ps1 -Game mkda -Matches 3
  powershell -File tools\koth-e2e.ps1 -Game mkd -Matches 4 -Snaps -Checkout C:\path\to\the\main\checkout
#>
param(
    [int]$Seconds = 1800,
    [string]$Game = 'mkda',      # mkda, mkd, mka
    [int]$Matches = 3,           # lobby matches before the launchers stop readying (three show the line go round)
    [int]$Port = 8677,           # the server's TCP port (UDP Port + 1), the games' Port + 10 to + 13
    [string]$Checkout = '',      # where the built games and their assets, Qt and the dev certificate are (default:
                                 # this checkout; a worktree has none of them)
    [string]$Launcher = '',      # default: build\launcher\MK3DLauncher.exe of this checkout
    [switch]$Snaps
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Checkout) { $Checkout = $root }
if (-not $Launcher) { $Launcher = Join-Path $root 'build\launcher\MK3DLauncher.exe' }
$work = Join-Path $root ($(if ($Port -eq 8677) { 'build\koth-e2e' } else { "build\koth-e2e-$Port" }))
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force -Path "$work\a", "$work\b", "$work\c", "$work\d", "$work\server", "$work\snaps" | Out-Null
Copy-Item "$Checkout\build\server-dev\server-cert.pem", "$Checkout\build\server-dev\server-key.pem" "$work\server\"
$fp = (Get-Content "$Checkout\build\launcher-dev\mk3d-launcher.ini" | Where-Object { $_ -like 'fingerprint=*' }) -replace '^fingerprint=', ''
$x = $Game.ToUpper()
$exe = "$Checkout\recomp\$Game\out\build\release\$x.exe"
# the launchers nowhere on the screen: Qt's offscreen platform, which windeployqt leaves out
$plugin = Join-Path (Split-Path -Parent $Launcher) 'platforms\qoffscreen.dll'
if (-not (Test-Path $plugin)) { Copy-Item "$Checkout\external\qt\6.12.0\msvc2022_64\plugins\platforms\qoffscreen.dll" $plugin }
$i = 0
foreach ($who in 'a', 'b', 'c', 'd') {
    $ini = @"
[server]
address=127.0.0.1:$Port
fingerprint=$fp
auto_connect=true

[games]
$Game\exe=$($exe -replace '\\', '/')

[online]
region=na
max_ping=0
delay=1
game_port=$($Port + 10 + $i)
pick_in_game=false
practice_while_waiting=false
search_again=false
hide_ip=false
match_sound=false

[account]
remember=false

[display]
mode=windowed
resolution=960x720

[dev]
udp_loopback=true
background=true
"@
    Set-Content -Path "$work\$who\mk3d-launcher.ini" -Value $ini -Encoding ascii
    $i++
}
$py = (Get-Command python).Source
$server = Start-Process -FilePath $py -ArgumentList @('-m', 'server', '--host', '127.0.0.1', '--port', "$Port", '--udp-port', "$($Port + 1)", '--db', "`"$work\server\koth.sqlite`"", '--cert', "`"$work\server\server-cert.pem`"", '--key', "`"$work\server\server-key.pem`"") `
    -WorkingDirectory $root -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\server\out.txt" -RedirectStandardError "$work\server\err.txt"
Start-Sleep -Seconds 2
$server.PriorityClass = 'BelowNormal'
# every game attacks from 25 s after it starts, for about half an hour, so whoever plays, the fight ends (an environment
# variable holds 32767 characters at most: every half second, the times written short)
$attacks = & $py -I -c "import itertools; seq=['RIGHT:0.4','X','X','Y','A','B','X','Y','X+Y','LEFT:0.3','B','A']; print(','.join(f'{25+i*0.5:g}:{s}' for i,s in enumerate(itertools.islice(itertools.cycle(seq),3500))))"
$vars = @('MK3D_DEV_PASSWORD', "${x}_INPUT", "${x}_PADS", "${x}_EVENT_LOG", 'SDL_ASSERT', 'QT_QPA_PLATFORM', 'QT_QPA_FONTDIR',
          'XBE_AUDIO_MUTE', 'SDL_XINPUT_ENABLED', 'SDL_JOYSTICK_WGI', 'SDL_JOYSTICK_RAWINPUT', 'SDL_JOYSTICK_DIRECTINPUT',
          'SDL_JOYSTICK_HIDAPI', 'SDL_JOYSTICK_GAMEINPUT')
$env:MK3D_DEV_PASSWORD = 'koth-e2e-pass-123'  # throw-away accounts on a throw-away server
Set-Item "env:${x}_INPUT" $attacks
Set-Item "env:${x}_PADS" '0'
Set-Item "env:${x}_EVENT_LOG" '1'
$env:SDL_ASSERT = 'always_ignore'
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QPA_FONTDIR = "$env:WINDIR\Fonts"  # the offscreen platform finds no fonts by itself (the pictures' text)
# test games: muted, and never the user's controller (the launchers' own controller reading too)
foreach ($k in $vars | Where-Object { $_ -like 'SDL_JOYSTICK_*' -or $_ -eq 'SDL_XINPUT_ENABLED' }) { Set-Item "env:$k" '0' }
$env:XBE_AUDIO_MUTE = '1'
$stamp = Get-Date -Format 'HHmmss'
$common = @('--lobby-name', "koth$stamp", '--lobby-matches', "$Matches", '--game', $Game, '--mode', 'kombat')
function Start-Launcher($who, $user, $extra) {
    if ($Snaps) { $extra += @('--snap-dir', "`"$work\snaps\$who`"") }
    $p = Start-Process -FilePath $Launcher -ArgumentList (@('--config', "`"$work\$who\mk3d-launcher.ini`"", '--user', $user, '--register') + $extra) -PassThru
    Start-Sleep -Milliseconds 500
    $p.PriorityClass = 'BelowNormal'  # its games start below normal too
    $p
}
$users = @{ a = "HillA$stamp"; b = "HillB$stamp"; c = "HillC$stamp"; d = "HillD$stamp" }
$roster = @{ mkda = @('Scorpion', 'SubZero', 'Kenshi'); mkd = @('Scorpion', 'Kobra', 'Ermac'); mka = @('Scorpion', 'SubZero', 'Ermac') }[$Game]
$la = Start-Launcher 'a' $users.a (@('--lobby', 'host', '--koth', '--character', $roster[0], '--rate', '5') + $common)
Start-Sleep -Seconds 3
$lb = Start-Launcher 'b' $users.b (@('--lobby', 'join', '--character', $roster[1], '--rate', '7') + $common)
Start-Sleep -Seconds 4
$lc = Start-Launcher 'c' $users.c (@('--lobby', 'join', '--character', $roster[2], '--rate', '9') + $common)
"server pid $($server.Id), launchers A $($la.Id) B $($lb.Id) C $($lc.Id); accounts $($users.a) $($users.b) $($users.c)"
$ls = @($la, $lb, $lc)
$t0 = Get-Date
$seen = @{}
$doneAt = $null
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    foreach ($g in @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.ParentProcessId -in @($ls | ForEach-Object { $_.Id }) })) {
        $gp = Get-Process -Id $g.ProcessId -ErrorAction SilentlyContinue
        if ($gp -and $gp.PriorityClass -ne 'BelowNormal') { try { $gp.PriorityClass = 'BelowNormal' } catch {} }
    }
    foreach ($s in Get-ChildItem -Path "$work\a\sessions", "$work\b\sessions", "$work\c\sessions" -Directory -ErrorAction SilentlyContinue) {
        $f = Join-Path $s.FullName 'status.txt'
        if (-not (Test-Path $f)) { continue }
        $line = (Get-Content $f -TotalCount 1 -ErrorAction SilentlyContinue)
        $key = "$($s.Parent.Parent.Name)/$($s.Name)"
        if ($line -and $seen[$key] -ne $line) {
            $seen[$key] = $line
            "{0,6:N1}s {1}: {2}" -f ((Get-Date) - $t0).TotalSeconds, $key, $line
        }
    }
    $serverLog = @(Get-Content "$work\server\err.txt", "$work\server\out.txt" -ErrorAction SilentlyContinue)
    foreach ($ln in $serverLog | Select-String -Pattern 'lobby \w+: .*(king|rates|rated|did not ready|Next up|left)|RESULT|DISPUTE|ABANDONED|NO_CONTEST|DESYNC|cancelled|Traceback') {
        if (-not $seen[$ln.Line]) { $seen[$ln.Line] = 1; "{0,6:N1}s server: {1}" -f ((Get-Date) - $t0).TotalSeconds, ($ln.Line -replace '^\S+ \S+ \S+\s+', '') }
    }
    $results = @($serverLog | Select-String -Pattern 'RESULT match=').Count
    $closed = @($serverLog | Select-String -Pattern 'rated, ').Count
    if ($results -ge $Matches -and -not $doneAt) { $doneAt = Get-Date }
    # the last match's rating closes once its spectator rated (after their game showed its end), 40 s on at most
    if ($doneAt -and ($closed -ge $Matches -or ((Get-Date) - $doneAt).TotalSeconds -gt 60)) { Start-Sleep -Seconds 5; break }
    if (@($ls | Where-Object { $_.HasExited }).Count) { "a launcher exited"; break }
    Start-Sleep -Milliseconds 500
}
"--- the Create lobby window and the leaderboard (launcher D)"
# with -Snaps it pictures the Create lobby window (its King of the Hill box; with Online Practice picked, none), then
# the leaderboard in both orders
$ld = Start-Launcher 'd' $users.d @('--page', 'lobby', '--game', $Game, '--mode', 'kombat')
Start-Sleep -Seconds 14
if ($Snaps) { Get-ChildItem "$work\snaps\d" -ErrorAction SilentlyContinue | ForEach-Object { "  $($_.Name)" } }
"--- launcher logs (cli, rating, watch and match lines)"
foreach ($s in 'a', 'b', 'c') {
    Get-Content "$work\$s\mk3d-launcher.log" -ErrorAction SilentlyContinue | Select-String -Pattern 'cli:|match: (Lobby match|Watching|Done watching|Match over|Starting|The players finished|Match cancelled|Cannot)|error' | ForEach-Object { "[$s] $($_.Line)" }
}
"--- game sessions: match starts and watching"
foreach ($s in 'a', 'b', 'c') {
    Get-ChildItem -Path "$work\$s\sessions" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $log = Join-Path $_.FullName 'game.log'
        if (Test-Path $log) { Select-String -Path $log -Pattern 'link: start|watching|DESYNC|differ|leaving the match' | Select-Object -First 30 | ForEach-Object { "[$s] $($_.Line)" } }
    }
}
"--- server: lobby and result lines"
Get-Content "$work\server\err.txt", "$work\server\out.txt" -ErrorAction SilentlyContinue | Select-String -Pattern 'lobby |RESULT| go: |feed:' | ForEach-Object { $_.Line -replace '^\S+ \S+ \S+\s+', '' }
# close only what this run started: the launchers and their games, then the server
$all = @($la, $lb, $lc, $ld)
$games = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.ParentProcessId -in @($all | ForEach-Object { $_.Id }) } | ForEach-Object { $_.ProcessId })
foreach ($p in $all) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 4
foreach ($id in @($all | ForEach-Object { $_.Id }) + $games) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
foreach ($k in $vars) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2  # processes just stopped are still there for a moment
"--- database"
& $py -I -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); print('respect', c.execute('SELECT a.name, r.points, r.ratings FROM respect r JOIN accounts a ON a.id=r.account_id ORDER BY a.name').fetchall()); print('ratings', c.execute('SELECT r.match_id, a.name, w.name, r.score FROM ratings r JOIN accounts a ON a.id=r.rater JOIN accounts w ON w.id=r.winner ORDER BY r.rated').fetchall()); print('matches', c.execute('SELECT id, status, winner, p1_rounds, p2_rounds FROM matches ORDER BY created').fetchall())" "$work\server\koth.sqlite"
"still running from this run: $(@(Get-Process -Id (@($all | ForEach-Object { $_.Id }) + $games + $server.Id) -ErrorAction SilentlyContinue).Count)"
"done"
