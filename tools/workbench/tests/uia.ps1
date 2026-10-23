# UI Automation helper for the UI tests: finds controls by their accessible name (a button's text,
# a dialog's title) in a top-level window and acts on them, as a person's click would.
#
#   uia.ps1 -Action list   -Window "Decomp Workbench" [-Depth 25]
#   uia.ps1 -Action invoke -Window "Decomp Workbench" -Name "Locate python.exe..."
#   uia.ps1 -Action settext -Window "Locate python.exe" -Name "File name:" -Text "C:\Python312\python.exe"
#   uia.ps1 -Action exists -Window "Run this compare command?"
#   uia.ps1 -Action gettext -Window "Run this compare command?" -Type Edit
#
# -Window matches a top-level window's title (substring), searched again until -Timeout seconds.
# -Name matches exactly first, then as a substring. Exit code 0 on success, 1 otherwise.
#
# The common file dialog (open a file, choose a folder) exposes its file-name field and Open /
# Select Folder button as panes with no Invoke or Value pattern. Those are set and clicked
# through the dialog's own controls.
param(
    [Parameter(Mandatory = $true)][string]$Action,
    [Parameter(Mandatory = $true)][string]$Window,
    [string]$Name = "",
    [string]$Text = "",
    [string]$Type = "",
    [int]$Depth = 30,
    [double]$Timeout = 10
)
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class UiaWin32 {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr child, string cls, string win);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
  public const int WM_SETTEXT = 0x000C;
  public const int BM_CLICK = 0x00F5;
}
"@
$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$global:uiaHwnd = [IntPtr]::Zero
$global:uiaWant = $Window

function Find-Hwnd {
    $global:uiaHwnd = [IntPtr]::Zero
    $cb = [UiaWin32+EnumProc]{
        param($h, $l)
        if (-not [UiaWin32]::IsWindowVisible($h)) { return $true }
        $sb = New-Object System.Text.StringBuilder 512
        [void][UiaWin32]::GetWindowText($h, $sb, 512)
        if ($sb.ToString().Contains($global:uiaWant)) { $global:uiaHwnd = $h; return $false }
        return $true
    }
    [UiaWin32]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $global:uiaHwnd
}

function ClassName($hwnd) {
    if ($hwnd -eq [IntPtr]::Zero) { return "" }
    $sb = New-Object System.Text.StringBuilder 128
    [void][UiaWin32]::GetClassName($hwnd, $sb, 128)
    return $sb.ToString()
}

function Find-Window {
    $end = (Get-Date).AddSeconds($Timeout)
    do {
        $hwnd = Find-Hwnd
        if ($hwnd -ne [IntPtr]::Zero) { return $AE::FromHandle($hwnd) }
        foreach ($w in $AE::RootElement.FindAll($TS::Children, [System.Windows.Automation.Condition]::TrueCondition)) {
            if ($w.Current.Name -like "*$Window*") { return $w }
            # dialogs owned by a window may sit under it
            foreach ($c in $w.FindAll($TS::Children, [System.Windows.Automation.Condition]::TrueCondition)) {
                if ($c.Current.ControlType -eq [System.Windows.Automation.ControlType]::Window -and $c.Current.Name -like "*$Window*") { return $c }
            }
        }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $end)
    return $null
}

function Dialog-Edit($dlg) {
    # open-file dialog: the name sits in the edit inside combo 1148. folder dialog: edit 1152.
    $combo = [UiaWin32]::GetDlgItem($dlg, 1148)
    if ($combo -ne [IntPtr]::Zero) {
        $inner = [UiaWin32]::FindWindowEx($combo, [IntPtr]::Zero, "ComboBox", $null)
        if ($inner -eq [IntPtr]::Zero) { $inner = $combo }
        $edit = [UiaWin32]::FindWindowEx($inner, [IntPtr]::Zero, "Edit", $null)
        if ($edit -ne [IntPtr]::Zero) { return $edit }
    }
    return [UiaWin32]::GetDlgItem($dlg, 1152)
}

function Click-Button($hwnd) {
    if ((ClassName $hwnd) -ne "Button") { return $false }
    [void][UiaWin32]::SendMessage($hwnd, [UiaWin32]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
    return $true
}

function Can-Click($e) {
    $p = $null
    return $e.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$p) -or
           $e.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$p) -or
           $e.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$p)
}

function Pick-Control($items) {
    $items = @($items | Where-Object { $_ })
    if ($items.Count -eq 0) { return $null }
    if ($script:PreferClick) {
        $clickable = @($items | Where-Object { Can-Click $_ })
        if ($clickable.Count -gt 0) { return $clickable[0] }
    }
    return $items[0]
}

$script:PreferClick = $false

function Find-Control($root) {
    $end = (Get-Date).AddSeconds($Timeout)
    do {
        $all = $root.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
        $types = @($Type -split "\|" | Where-Object { $_ } | ForEach-Object { "ControlType.$_" })
        $typed = @($all | Where-Object { $types.Count -eq 0 -or $types -contains $_.Current.ControlType.ProgrammaticName })
        if (-not $Name) { $hit = $typed | Select-Object -First 1 }
        else {
            $hit = Pick-Control ($typed | Where-Object { $_.Current.Name -eq $Name -and $_.Current.IsEnabled })
            if (-not $hit) { $hit = Pick-Control ($typed | Where-Object { $_.Current.Name -like "*$Name*" -and $_.Current.IsEnabled }) }
        }
        if ($hit) { return $hit }
        Start-Sleep -Milliseconds 200
    } while ((Get-Date) -lt $end)
    return $null
}

$w = Find-Window
if (-not $w) { Write-Output "no window '$Window'"; exit 1 }
switch ($Action) {
    "exists" { Write-Output $w.Current.Name; exit 0 }
    "list" {
        $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
        function Walk($e, $d) {
            if ($d -gt $Depth) { return }
            $c = $walker.GetFirstChild($e)
            while ($c) {
                $n = $c.Current.Name
                if ($n) { Write-Output (("  " * $d) + $c.Current.ControlType.ProgrammaticName.Replace("ControlType.", "") + ": " + $n) }
                Walk $c ($d + 1)
                $c = $walker.GetNextSibling($c)
            }
        }
        Walk $w 0
        exit 0
    }
    "invoke" {
        $dlg = [IntPtr]$w.Current.NativeWindowHandle
        if ((ClassName $dlg) -eq "#32770") {
            $id = 0
            if ($Name -eq "Open" -or $Name -eq "Select Folder") { $id = 1 }
            elseif ($Name -eq "Cancel") { $id = 2 }
            if ($id -and (Click-Button ([UiaWin32]::GetDlgItem($dlg, $id)))) { Write-Output "invoked $Name"; exit 0 }
        }
        $script:PreferClick = $true
        $c = Find-Control $w
        if (-not $c) { Write-Output "no control '$Name' in '$($w.Current.Name)'"; exit 1 }
        $p = $null
        if ($c.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$p)) { $p.Invoke() }
        elseif ($c.TryGetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern, [ref]$p)) { $p.Toggle() }
        elseif ($c.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern, [ref]$p)) { $p.Select() }
        elseif (Click-Button ([IntPtr]$c.Current.NativeWindowHandle)) { }
        else { Write-Output "'$Name' can't be clicked"; exit 1 }
        Write-Output "invoked $($c.Current.Name)"
        exit 0
    }
    "settext" {
        $dlg = [IntPtr]$w.Current.NativeWindowHandle
        if ((ClassName $dlg) -eq "#32770") {
            $edit = Dialog-Edit $dlg
            if ($edit -ne [IntPtr]::Zero) {
                [void][UiaWin32]::SendMessage($edit, [UiaWin32]::WM_SETTEXT, [IntPtr]::Zero, $Text)
                Write-Output "set dialog text"
                exit 0
            }
        }
        $c = Find-Control $w
        if (-not $c) { Write-Output "no field '$Name' in '$($w.Current.Name)'"; exit 1 }
        $p = $null
        if (-not $c.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$p)) { Write-Output "'$Name' takes no text"; exit 1 }
        $p.SetValue($Text)
        Write-Output "set $($c.Current.Name)"
        exit 0
    }
    "gettext" {
        $c = Find-Control $w
        if (-not $c) { Write-Output "no field '$Name' in '$($w.Current.Name)'"; exit 1 }
        $p = $null
        if ($c.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$p)) { Write-Output $p.Current.Value; exit 0 }
        if ($c.TryGetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern, [ref]$p)) { Write-Output $p.DocumentRange.GetText(-1); exit 0 }
        Write-Output $c.Current.Name
        exit 0
    }
}
Write-Output "unknown action $Action"
exit 1
