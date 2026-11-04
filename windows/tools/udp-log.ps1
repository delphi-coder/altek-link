param([int]$Port = 45879)
$ErrorActionPreference = 'Stop'
$logDir = Join-Path $PSScriptRoot 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$logPath = Join-Path $logDir ("altek-" + $stamp + '.log')
$utf8 = New-Object System.Text.UTF8Encoding($false)
$writer = New-Object System.IO.StreamWriter($logPath, $false, $utf8)
$writer.AutoFlush = $true
$udp = New-Object System.Net.Sockets.UdpClient
try {
    $udp.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, $Port)))
    $udp.Client.ReceiveTimeout = 1000
    Write-Host "ALTEK Link UDP log receiver on port $Port" -ForegroundColor Cyan
    Write-Host "Enable diagnostic logs in Settings and set the receiver IPv4 to this PC."
    Write-Host "Saving: $logPath"
    Write-Host 'Ctrl+C to stop. All devices are shown together, identified by model / ID / source IP.'
    while ($true) {
        $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        try { $bytes = $udp.Receive([ref]$remote) }
        catch [System.Net.Sockets.SocketException] {
            if ($_.Exception.SocketErrorCode -eq [System.Net.Sockets.SocketError]::TimedOut) { continue }
            throw
        }
        $body = $utf8.GetString($bytes) -replace '[\x00-\x08\x0B-\x1F\x7F]', '?'
        $line = '{0} {1} {2}' -f (Get-Date -Format 'HH:mm:ss.fff'), $remote.Address, $body
        Write-Host $line
        $writer.WriteLine($line)
    }
}
finally { $writer.Dispose(); $udp.Close() }
