param(
    [Parameter(Mandatory=$true)]
    [string]$OutputFile,
    [switch]$SelfTest
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$form = New-Object System.Windows.Forms.Form
$form.Text = "NextGen Match Lab - Select Time Control"
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.ClientSize = New-Object System.Drawing.Size(520, 430)
$form.BackColor = [System.Drawing.Color]::FromArgb(23, 27, 34)
$form.ForeColor = [System.Drawing.Color]::White
$form.Font = New-Object System.Drawing.Font("Segoe UI", 10)

$title = New-Object System.Windows.Forms.Label
$title.Text = "Choose your time control"
$title.Location = New-Object System.Drawing.Point(22, 18)
$title.Size = New-Object System.Drawing.Size(470, 34)
$title.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 18)
$title.ForeColor = [System.Drawing.Color]::White
$form.Controls.Add($title)

$subtitle = New-Object System.Windows.Forms.Label
$subtitle.Text = "Click a preset or enter a custom clock. Format: minutes + increment seconds."
$subtitle.Location = New-Object System.Drawing.Point(24, 58)
$subtitle.Size = New-Object System.Drawing.Size(470, 38)
$subtitle.ForeColor = [System.Drawing.Color]::FromArgb(190, 198, 210)
$form.Controls.Add($subtitle)

$section = New-Object System.Windows.Forms.Label
$section.Text = "PRESETS"
$section.Location = New-Object System.Drawing.Point(24, 108)
$section.Size = New-Object System.Drawing.Size(450, 22)
$section.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 9)
$section.ForeColor = [System.Drawing.Color]::FromArgb(133, 170, 220)
$form.Controls.Add($section)

$controls = @(
    @{ Label = "Bullet   1+0"; Value = "1+0" },
    @{ Label = "Bullet   2+1"; Value = "2+1" },
    @{ Label = "Blitz    3+0"; Value = "3+0" },
    @{ Label = "Blitz    3+2"; Value = "3+2" },
    @{ Label = "Blitz    5+0"; Value = "5+0" },
    @{ Label = "Blitz    5+3"; Value = "5+3" },
    @{ Label = "Rapid   10+0"; Value = "10+0" },
    @{ Label = "Rapid   10+5"; Value = "10+5" },
    @{ Label = "Rapid   15+10"; Value = "15+10" }
)

$selection = @{ Value = $null }
$left = 24
$top = 140
$buttonWidth = 145
$buttonHeight = 48
$gapX = 12
$gapY = 10
for ($i = 0; $i -lt $controls.Count; $i++) {
    $item = $controls[$i]
    $col = $i % 3
    $row = [math]::Floor($i / 3)
    $button = New-Object System.Windows.Forms.Button
    $button.Text = $item.Label
    $button.Tag = $item.Value
    $button.Location = New-Object System.Drawing.Point(($left + $col * ($buttonWidth + $gapX)), ($top + $row * ($buttonHeight + $gapY)))
    $button.Size = New-Object System.Drawing.Size($buttonWidth, $buttonHeight)
    $button.FlatStyle = "Flat"
    $button.FlatAppearance.BorderColor = [System.Drawing.Color]::FromArgb(75, 88, 105)
    $button.BackColor = [System.Drawing.Color]::FromArgb(42, 51, 64)
    $button.ForeColor = [System.Drawing.Color]::White
    $button.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 10)
    $button.Add_Click({
        $selection.Value = [string]$this.Tag
        $form.Close()
    })
    $form.Controls.Add($button)
}

$customLabel = New-Object System.Windows.Forms.Label
$customLabel.Text = "CUSTOM TIME CONTROL"
$customLabel.Location = New-Object System.Drawing.Point(24, 322)
$customLabel.Size = New-Object System.Drawing.Size(220, 22)
$customLabel.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 9)
$customLabel.ForeColor = [System.Drawing.Color]::FromArgb(133, 170, 220)
$form.Controls.Add($customLabel)

$custom = New-Object System.Windows.Forms.TextBox
$custom.Text = "3+0"
$custom.Location = New-Object System.Drawing.Point(24, 350)
$custom.Size = New-Object System.Drawing.Size(280, 34)
$custom.Font = New-Object System.Drawing.Font("Consolas", 13)
$form.Controls.Add($custom)

$start = New-Object System.Windows.Forms.Button
$start.Text = "Start custom"
$start.Location = New-Object System.Drawing.Point(320, 348)
$start.Size = New-Object System.Drawing.Size(145, 38)
$start.FlatStyle = "Flat"
$start.BackColor = [System.Drawing.Color]::FromArgb(43, 117, 83)
$start.ForeColor = [System.Drawing.Color]::White
$start.Font = New-Object System.Drawing.Font("Segoe UI Semibold", 10)
$start.Add_Click({
    $value = $custom.Text.Trim()
    if ($value -notmatch '^\s*(\d+(?:\.\d+)?)\s*\+\s*(\d+(?:\.\d+)?)\s*$' -or [double]$Matches[1] -le 0) {
        [System.Windows.Forms.MessageBox]::Show(
            "Enter a valid clock such as 3+0, 5+3 or 10+5.",
            "Invalid time control",
            [System.Windows.Forms.MessageBoxButtons]::OK,
            [System.Windows.Forms.MessageBoxIcon]::Warning
        ) | Out-Null
        return
    }
    $selection.Value = $value
    $form.Close()
})
$form.Controls.Add($start)

$cancel = New-Object System.Windows.Forms.Button
$cancel.Text = "Cancel"
$cancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
$cancel.Location = New-Object System.Drawing.Point(420, 400)
$cancel.Size = New-Object System.Drawing.Size(75, 25)
$cancel.FlatStyle = "Flat"
$cancel.ForeColor = [System.Drawing.Color]::FromArgb(190, 198, 210)
$form.Controls.Add($cancel)
$form.CancelButton = $cancel

if ($SelfTest) {
    # Build all WinForms controls without opening a visible dialog.
    $selection.Value = "3+0"
} else {
    [void]$form.ShowDialog()
}
if ([string]::IsNullOrWhiteSpace($selection.Value)) {
    exit 2
}
[System.IO.File]::WriteAllText($OutputFile, $selection.Value, [System.Text.Encoding]::ASCII)
exit 0
