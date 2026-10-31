# blitz86 settings for b86.bat. A variable already set in the PowerShell window
# wins, e.g.  $env:B86_PORT = 'COM7'; .\b86.bat hw run
# '.\b86.bat config' prints the effective values and where they came from.
@{
    B86_MICRODOS = 'C:\microDOS'   # microDOS checkout (Pico/Pi build trees, md.bat)
    B86_PORT     = ''              # serial port; empty = each tool's own default
                                   # (dev tools: COM5; hw / jit-pico: auto-detect)
}
