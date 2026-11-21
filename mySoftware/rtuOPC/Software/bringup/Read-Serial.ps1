[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^COM[0-9]+$')]
    [string]$Port,

    [ValidateRange(1200, 3000000)]
    [int]$BaudRate = 115200,

    [ValidateRange(1, 600)]
    [int]$DurationSeconds = 180,

    [Parameter(Mandatory)]
    [string]$LogPath
)

$ErrorActionPreference = 'Stop'
$absoluteLogPath = [System.IO.Path]::GetFullPath($LogPath)
$logDirectory = [System.IO.Path]::GetDirectoryName($absoluteLogPath)
[System.IO.Directory]::CreateDirectory($logDirectory) | Out-Null

$serialPort = [System.IO.Ports.SerialPort]::new(
    $Port,
    $BaudRate,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One
)
$serialPort.Handshake = [System.IO.Ports.Handshake]::None
$serialPort.DtrEnable = $false
$serialPort.RtsEnable = $false
$serialPort.ReadTimeout = 200
$serialPort.Encoding = [System.Text.Encoding]::ASCII
$logWriter = $null
$receivedBytes = 0
$buffer = [byte[]]::new(8192)

try {
    $serialPort.Open()
    $logWriter = [System.IO.FileStream]::new(
        $absoluteLogPath,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::Read
    )
    Write-Output "SERIAL_OPEN $Port $BaudRate 8N1"
    Write-Output "CAPTURE_STARTED $([DateTimeOffset]::Now.ToString('o'))"
    Write-Output "CAPTURE_LOG $absoluteLogPath"
    $captureTimer = [System.Diagnostics.Stopwatch]::StartNew()

    while ($captureTimer.Elapsed.TotalSeconds -lt $DurationSeconds) {
        try {
            $count = $serialPort.Read($buffer, 0, $buffer.Length)
        }
        catch [System.TimeoutException] {
            continue
        }

        $logWriter.Write($buffer, 0, $count)
        $logWriter.Flush()
        $receivedBytes += $count
        [Console]::Write([System.Text.Encoding]::ASCII.GetString($buffer, 0, $count))
    }
}
finally {
    if ($null -ne $logWriter) {
        $logWriter.Dispose()
    }
    if ($serialPort.IsOpen) {
        $serialPort.Close()
    }
    $serialPort.Dispose()
    Write-Output "`nCAPTURE_BYTES $receivedBytes"
}
