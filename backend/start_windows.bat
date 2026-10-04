@echo off
setlocal EnableExtensions
cd /d "%~dp0"

title PS4 Hybrid Browser Backend v7.3.1
set "LOG=%~dp0backend_start.log"

> "%LOG%" echo ============================================================
>>"%LOG%" echo PS4 Hybrid Browser Backend v7.3.1
>>"%LOG%" echo Started: %DATE% %TIME%
>>"%LOG%" echo Folder: %CD%
>>"%LOG%" echo ============================================================

echo.
echo ============================================================
echo   PS4 Hybrid Browser Backend v7.3.1
echo ============================================================
echo.
echo Esta janela DEVE ficar aberta enquanto voce usa o navegador no PS4.
echo Se houver erro, ela NAO vai fechar sozinha.
echo O diagnostico sera salvo em:
echo   %LOG%
echo.

where node >nul 2>&1
if errorlevel 1 (
    echo [ERRO] Node.js nao foi encontrado.
    >>"%LOG%" echo [ERRO] Node.js nao foi encontrado no PATH.
    echo.
    echo Instale Node.js 18 ou superior e execute este arquivo novamente.
    echo Depois da instalacao, feche e abra esta pasta novamente.
    goto :HOLD_ERROR
)

for /f "delims=" %%V in ('node --version 2^>^&1') do set "NODEVER=%%V"
for /f "delims=" %%V in ('npm --version 2^>^&1') do set "NPMVER=%%V"
echo Node: %NODEVER%
echo npm : %NPMVER%
>>"%LOG%" echo Node: %NODEVER%
>>"%LOG%" echo npm : %NPMVER%

echo.
if not exist "node_modules\" (
    echo [1/3] Instalando dependencias do backend...
    >>"%LOG%" echo [1/3] npm install
    call npm install >>"%LOG%" 2>&1
    if errorlevel 1 (
        echo [ERRO] npm install falhou.
        echo Veja backend_start.log.
        goto :HOLD_ERROR
    )
) else (
    echo [1/3] Dependencias ja instaladas.
    >>"%LOG%" echo [1/3] node_modules ja existe
)

echo.
echo [2/3] Verificando Chromium do Playwright...
>>"%LOG%" echo [2/3] npx playwright install chromium
call npx playwright install chromium >>"%LOG%" 2>&1
if errorlevel 1 (
    echo [ERRO] Nao foi possivel instalar/verificar o Chromium.
    echo Veja backend_start.log.
    goto :HOLD_ERROR
)

echo.
echo [3/3] Iniciando backend...
echo.
echo Quando estiver pronto voce deve ver mensagens como:
echo   [Hybrid] HTTP port: 32124
echo   [Hybrid] Discovery UDP: 32123
echo.
echo NAO feche esta janela.
echo ============================================================
echo.
>>"%LOG%" echo [3/3] node server.js

node server.js >>"%LOG%" 2>&1
set "RC=%ERRORLEVEL%"

echo.
echo ============================================================
echo [ERRO] O backend encerrou. Codigo: %RC%
echo Isso NAO e o comportamento normal.
echo Abra backend_start.log e envie o conteudo para o ChatGPT.
echo ============================================================
>>"%LOG%" echo Backend terminou com codigo %RC%
goto :HOLD_ERROR

:HOLD_ERROR
echo.
echo A janela ficara aberta para voce ler o erro.
echo Para fechar manualmente, digite EXIT e pressione Enter.
echo.
cmd /k
