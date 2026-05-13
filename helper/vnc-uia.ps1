# vnc-uia.ps1 — UI Automation helper for VNC MCP Server
#
# Provides accessibility tree walking, element finding, and interaction
# using .NET's System.Windows.Automation (UIAutomationClient).
#
# Usage:
#   vnc-uia.ps1 -Mode tree [-Depth 3] [-Pid 1234]
#   vnc-uia.ps1 -Mode find -Name "Save" [-ControlType Button]
#   vnc-uia.ps1 -Mode click -Name "OK" [-ControlType Button]
#   vnc-uia.ps1 -Mode text -Name "File name:" [-ControlType Edit]
#   vnc-uia.ps1 -Mode focused
#
# Output: JSON to stdout
# Errors: JSON with "error" key to stdout (exit code 1)
#
# Copyright (c) 2026, The Daniel Morante Company, Inc.
# BSD 2-Clause License

param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("tree", "find", "click", "text", "focused")]
    [string]$Mode,

    [int]$Depth = 3,
    [int]$Pid = 0,
    [string]$Name = "",
    [string]$AutomationId = "",
    [string]$ControlType = "",
    [int]$MaxResults = 50
)

$ErrorActionPreference = "Stop"

try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
} catch {
    Write-Output ('{"error":"Failed to load UIAutomation assemblies: ' + $_.Exception.Message + '"}')
    exit 1
}

function Get-ControlTypeName($element) {
    $ct = $element.Current.ControlType
    if ($ct -eq $null) { return "Unknown" }
    return $ct.ProgrammaticName -replace "ControlType\.", ""
}

function Get-ElementInfo($element) {
    try {
        $rect = $element.Current.BoundingRectangle
        $info = @{
            name = if ($element.Current.Name) { $element.Current.Name } else { "" }
            controlType = Get-ControlTypeName $element
            automationId = if ($element.Current.AutomationId) { $element.Current.AutomationId } else { "" }
            className = if ($element.Current.ClassName) { $element.Current.ClassName } else { "" }
            isEnabled = $element.Current.IsEnabled
            x = [int]$rect.X
            y = [int]$rect.Y
            w = [int]$rect.Width
            h = [int]$rect.Height
        }
        return $info
    } catch {
        return @{ name = "(error)"; controlType = "Unknown"; error = $_.Exception.Message }
    }
}

function Walk-Tree($element, $currentDepth, $maxDepth) {
    if ($currentDepth -gt $maxDepth) { return $null }

    $info = Get-ElementInfo $element
    $children = @()

    if ($currentDepth -lt $maxDepth) {
        try {
            $walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
            $child = $walker.GetFirstChild($element)
            $count = 0
            while ($child -ne $null -and $count -lt 100) {
                $childNode = Walk-Tree $child ($currentDepth + 1) $maxDepth
                if ($childNode -ne $null) {
                    $children += $childNode
                }
                $child = $walker.GetNextSibling($child)
                $count++
            }
        } catch {
            # Silently skip elements that throw
        }
    }

    if ($children.Count -gt 0) {
        $info["children"] = $children
    }

    return $info
}

function Find-Elements($name, $automationId, $controlTypeName) {
    $root = [System.Windows.Automation.AutomationElement]::RootElement
    $conditions = @()

    if ($name) {
        $conditions += New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::NameProperty, $name)
    }

    if ($automationId) {
        $conditions += New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $automationId)
    }

    if ($controlTypeName) {
        $ctType = [System.Windows.Automation.ControlType]::$controlTypeName
        if ($ctType) {
            $conditions += New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $ctType)
        }
    }

    if ($conditions.Count -eq 0) {
        Write-Output '{"error":"Must specify -Name or -AutomationId"}'
        exit 1
    }

    $condition = $null
    if ($conditions.Count -eq 1) {
        $condition = $conditions[0]
    } else {
        $condition = New-Object System.Windows.Automation.AndCondition($conditions)
    }

    $elements = $root.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        $condition)

    return $elements
}

# ================================================================
# Mode: tree — dump the accessibility tree
# ================================================================
if ($Mode -eq "tree") {
    $root = $null
    if ($Pid -gt 0) {
        # Find window by PID
        $condition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $Pid)
        $root = [System.Windows.Automation.AutomationElement]::RootElement.FindFirst(
            [System.Windows.Automation.TreeScope]::Children, $condition)
        if ($root -eq $null) {
            Write-Output ('{"error":"No window found for PID ' + $Pid + '"}')
            exit 1
        }
    } else {
        # Use the foreground window
        Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class WinAPI {
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
}
"@
        $hwnd = [WinAPI]::GetForegroundWindow()
        if ($hwnd -ne [IntPtr]::Zero) {
            try {
                $root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
            } catch {
                $root = [System.Windows.Automation.AutomationElement]::RootElement
            }
        } else {
            $root = [System.Windows.Automation.AutomationElement]::RootElement
        }
    }

    $tree = Walk-Tree $root 0 $Depth
    $json = $tree | ConvertTo-Json -Depth 20 -Compress
    Write-Output $json
    exit 0
}

# ================================================================
# Mode: find — find elements by name/automationId/controlType
# ================================================================
if ($Mode -eq "find") {
    $elements = Find-Elements $Name $AutomationId $ControlType
    $results = @()
    $count = 0
    foreach ($el in $elements) {
        if ($count -ge $MaxResults) { break }
        $results += Get-ElementInfo $el
        $count++
    }
    $output = @{ count = $results.Count; elements = $results }
    $json = $output | ConvertTo-Json -Depth 10 -Compress
    Write-Output $json
    exit 0
}

# ================================================================
# Mode: click — find element and invoke/click it
# ================================================================
if ($Mode -eq "click") {
    $elements = Find-Elements $Name $AutomationId $ControlType
    if ($elements.Count -eq 0) {
        Write-Output '{"error":"Element not found"}'
        exit 1
    }

    $el = $elements[0]

    # Try InvokePattern first (buttons, links, menu items)
    try {
        $pattern = $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
        $pattern.Invoke()
        $info = Get-ElementInfo $el
        $output = @{ action = "invoked"; element = $info }
        Write-Output ($output | ConvertTo-Json -Depth 5 -Compress)
        exit 0
    } catch {}

    # Try TogglePattern (checkboxes)
    try {
        $pattern = $el.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern)
        $pattern.Toggle()
        $info = Get-ElementInfo $el
        $output = @{ action = "toggled"; element = $info }
        Write-Output ($output | ConvertTo-Json -Depth 5 -Compress)
        exit 0
    } catch {}

    # Try SelectionItemPattern (radio buttons, list items)
    try {
        $pattern = $el.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern)
        $pattern.Select()
        $info = Get-ElementInfo $el
        $output = @{ action = "selected"; element = $info }
        Write-Output ($output | ConvertTo-Json -Depth 5 -Compress)
        exit 0
    } catch {}

    # Try ExpandCollapsePattern (dropdowns, tree items)
    try {
        $pattern = $el.GetCurrentPattern([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
        $state = $pattern.Current.ExpandCollapseState
        if ($state -eq [System.Windows.Automation.ExpandCollapseState]::Collapsed) {
            $pattern.Expand()
            $action = "expanded"
        } else {
            $pattern.Collapse()
            $action = "collapsed"
        }
        $info = Get-ElementInfo $el
        $output = @{ action = $action; element = $info }
        Write-Output ($output | ConvertTo-Json -Depth 5 -Compress)
        exit 0
    } catch {}

    # Fallback: try to set focus and use the bounding rectangle center
    try {
        $el.SetFocus()
        $info = Get-ElementInfo $el
        $output = @{ action = "focused"; element = $info; note = "No click pattern available, element focused instead" }
        Write-Output ($output | ConvertTo-Json -Depth 5 -Compress)
        exit 0
    } catch {
        $info = Get-ElementInfo $el
        $output = @{ action = "none"; element = $info; error = "No supported interaction pattern found" }
        Write-Output ($output | ConvertTo-Json -Depth 5 -Compress)
        exit 1
    }
}

# ================================================================
# Mode: text — get text/value from an element
# ================================================================
if ($Mode -eq "text") {
    $elements = Find-Elements $Name $AutomationId $ControlType
    if ($elements.Count -eq 0) {
        Write-Output '{"error":"Element not found"}'
        exit 1
    }

    $el = $elements[0]
    $info = Get-ElementInfo $el
    $textValue = ""

    # Try ValuePattern (text boxes, edits)
    try {
        $pattern = $el.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
        $textValue = $pattern.Current.Value
    } catch {
        # Try TextPattern (rich text controls)
        try {
            $pattern = $el.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
            $range = $pattern.DocumentRange
            $textValue = $range.GetText(-1)
        } catch {
            # Fall back to Name property
            $textValue = $el.Current.Name
        }
    }

    $output = @{ text = $textValue; element = $info }
    Write-Output ($output | ConvertTo-Json -Depth 5 -Compress)
    exit 0
}

# ================================================================
# Mode: focused — get info about the currently focused element
# ================================================================
if ($Mode -eq "focused") {
    try {
        $focused = [System.Windows.Automation.AutomationElement]::FocusedElement
        if ($focused -eq $null) {
            Write-Output '{"error":"No focused element"}'
            exit 1
        }
        $info = Get-ElementInfo $focused
        $json = $info | ConvertTo-Json -Depth 5 -Compress
        Write-Output $json
        exit 0
    } catch {
        Write-Output ('{"error":"' + $_.Exception.Message + '"}')
        exit 1
    }
}
