# PS4 Hybrid Browser Backend v7

Roda Chromium/Playwright em um PC e entrega ao PS4 uma interface simples.

Windows: instale Node.js 18+, execute start_windows.bat e permita as portas na rede privada.
Linux/macOS: chmod +x start_linux.sh && ./start_linux.sh

Portas:
- UDP 32123: descoberta automática
- TCP 32124: interface e proxy de downloads

O PS4 e o computador precisam estar na mesma rede. Na primeira execução, Playwright baixa o Chromium.
A interface inclui URL/pesquisa, voltar, avançar, recarregar, scroll, envio de texto e "Baixar imagem".
Quando um download aparece, clique em BAIXAR NO PS4.

Não exponha a porta 32124 diretamente à Internet sem autenticação.
