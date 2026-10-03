# PS4 Browser Downloader

Homebrew para PS4 com navegador simples e downloads PKG entregues ao BGFT para continuarem em segundo plano.

## Objetivo

- abrir sites diretamente no PS4;
- usar o WebBrowserDialog do sistema, evitando portar Chromium inteiro;
- interceptar links diretos `.pkg` quando o callback do navegador estiver disponível;
- validar o cabeçalho PKG antes de criar a tarefa;
- registrar o download no BGFT para que ele continue depois que o app for fechado;
- exibir diagnostico claro em vez de encerrar com CE-34878-0 quando algum modulo/servico falhar.

> Use apenas com homebrew, backups proprios e conteudo que voce tenha direito de baixar/instalar.

## Build

Cada push em `main` executa o GitHub Actions e publica o artefato `PS4-Browser-Downloader-PS4` contendo o PKG e SHA-256.
