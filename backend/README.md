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


## v7.1 auditado

- A descoberta usa o IP de origem do pacote UDP, evitando escolher VPN/VirtualBox/Tailscale.
- O PS4 valida /health antes de abrir o backend.
- Se o backend nao for encontrado, nao volta silenciosamente ao navegador local antigo.
- Downloads iniciados por sites sao concluidos no Chromium antes do handoff. Isso evita perder URLs temporarias/de uso unico.
- Para arquivos comuns em background, GoldHEN BinLoader precisa estar ativo na porta 9090.
- Para PKG, o cliente detecta o magic real do pacote e usa BGFT mesmo sem extensao .pkg.


## v7.2 - interface compatível com WebKit antigo

A interface do PS4 agora é HTML simples gerado no servidor, sem depender de XHR, fetch,
setInterval ou CSS moderno. O Chromium continua no PC; o PS4 apenas exibe formulários
básicos e uma captura PNG clicável.

Se a interface abrir corretamente você verá fundo branco, título "PS4 Hybrid Browser v7.2",
barra de URL, botões e a imagem da página Chromium.
