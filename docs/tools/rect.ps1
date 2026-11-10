# Prints the screen rectangle (left top right bottom) of each control in a top-level window whose title
# contains -Window, one per line: controls named -Name, or (with -Value) combo boxes and fields showing
# that value. capture.py uses it to find a panel's controls on screen.
param(
    [Parameter(Mandatory = $true)][string]$Window,
    [string]$Name = "",
    [string]$Value = ""
)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$A = [System.Windows.Automation.AutomationElement]
$root = $A::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition($A::ControlTypeProperty, [System.Windows.Automation.ControlType]::Window)
$win = $null
foreach ($w in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $cond)) {
    if ($w.Current.Name -like "*$Window*") { $win = $w; break }
}
if (-not $win) { exit 1 }
if ($Value) {
    $found = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
} else {
    $byName = New-Object System.Windows.Automation.PropertyCondition($A::NameProperty, $Name)
    $found = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $byName)
}
foreach ($e in $found) {
    if ($Value) {
        $p = $null
        if (-not $e.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$p)) { continue }
        if ($p.Current.Value -ne $Value) { continue }
    }
    $r = $e.Current.BoundingRectangle
    if ($r.Width -gt 0) {
        Write-Output ("{0} {1} {2} {3} {4}" -f [int]$r.Left, [int]$r.Top, [int]$r.Right, [int]$r.Bottom, $e.Current.ControlType.ProgrammaticName)
    }
}
