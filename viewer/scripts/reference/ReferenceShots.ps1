# ReferenceShots: the shot list reader shared by the WP-Q2 scripts (dot-sourced). The list is UTF-8 (Windows
# PowerShell 5.1 would read it in the ANSI code page by default) and every id must be one safe path and HTML token.
function Test-ReferenceName([string]$Name) {
    # Lower-case letters and digits joined by dashes (\z: no trailing newline), and not a Windows device name.
    $Name -cmatch '\A[a-z0-9]+(-[a-z0-9]+)*\z' -and $Name -notmatch '\A(con|prn|aux|nul|com\d|lpt\d)\z'
}
function Read-ReferenceShots([Parameter(Mandatory)][string]$Path) {
    $list = @(([System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8) | ConvertFrom-Json).shots)
    foreach ($s in $list) {
        if (-not (Test-ReferenceName "$($s.id)")) { throw "shot id '$($s.id)': lower-case letters and digits joined by dashes, not a device name" }
    }
    $dupes = $list.id | Group-Object | Where-Object Count -gt 1
    if ($dupes) { throw "shot ids used twice: $($dupes.Name -join ', ')" }
    $list
}
