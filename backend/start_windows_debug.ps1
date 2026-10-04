$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$log = Join-Path $PSScriptRoot "backend_start.log"

function Hold-Error([string]$Message) {
    Write-Host ""
    Write-Host $Message -ForegroundColor Red
    Write-Host "Log: $log" -ForegroundColor Yellow
    Write-Host "Esta janela ficara aberta. Feche somente depois de ler/copiar o erro."
    while ($true) { Start-Sleep -Seconds 3600 }
}

"PS4 Hybrid Browser Backend v7.3.1 - $(Get-Date)" | Out-File $log

try {
    $node = Get-Command node -ErrorAction Stop
} catch {
    Hold-Error "Node.js nao foi encontrado. Instale Node.js 18 ou superior e tente novamente."
}

Write-Host "Node:" (& node --version)
Write-Host "npm :" (& npm --version)

if (-not (Test-Path (Join-Path $PSScriptRoot "node_modules"))) {
    Write-Host "[1/3] Instalando dependencias..."
    & npm install *>> $log
    if ($LASTEXITCODE -ne 0) { Hold-Error "npm install falhou." }
} else {
    Write-Host "[1/3] Dependencias ja instaladas."
}

Write-Host "[2/3] Verificando Chromium..."
& npx playwright install chromium *>> $log
if ($LASTEXITCODE -ne 0) { Hold-Error "Playwright/Chromium falhou." }

Write-Host "[3/3] Iniciando backend. NAO feche esta janela."
& node server.js *>> $log
Hold-Error "O backend encerrou com codigo $LASTEXITCODE."
