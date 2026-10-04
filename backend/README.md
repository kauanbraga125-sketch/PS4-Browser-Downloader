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


## v7.3 - cliente grafico nativo

O PS4 nao usa mais WebBrowserDialog para mostrar a interface. Ele baixa /shot do backend,
decodifica o PNG e desenha diretamente em 1920x1080 via SDL. Controle:
- Analogico esquerdo: cursor
- X: clicar
- O ou L1: voltar
- R1: avancar
- Quadrado: recarregar
- D-pad cima/baixo: scroll
- R2: URL/pesquisa pelo teclado do PS4
- R3: digitar no campo focado da pagina
- Triangulo: baixar imagem sob o cursor
- Options: sair

Downloads concluidos pelo Chromium sao detectados automaticamente pelo cliente nativo
e enviados a BGFT (PKG) ou ao daemon de arquivos comuns.


## Inicializador Windows v7.3.1

Use `start_windows.bat`. A janela deve permanecer aberta enquanto o PS4 estiver usando o navegador.

Se Node.js, npm, Playwright ou Chromium falharem, o iniciador não fecha silenciosamente:
- mostra a etapa que falhou;
- grava tudo em `backend_start.log`;
- mantém uma janela de comando aberta para leitura.

Se o backend estiver realmente pronto, a janela não volta ao prompt e o log deve conter as mensagens de inicialização do servidor nas portas TCP 32124 e UDP 32123.


## v7.7 MAX performance

A rota de vídeo não faz mais polling HTTP por frame. O backend abre um stream TCP
persistente em HTTP_PORT+1 (32125 por padrão) e envia somente o frame JPEG mais recente.

O padrão de desempenho usa screencast Chromium 960x540 JPEG qualidade 35. O PS4:
- recebe frames no mesmo socket TCP;
- decodifica JPEG em thread separada;
- transfere ownership do buffer RGBA para a thread gráfica sem copiar o frame;
- atualiza uma textura Piglet/GLES e faz upscale para 1920x1080 na GPU;
- desenha cursor e apresenta a tela em ~60 Hz;
- executa navegação/scroll/Home em uma fila de comandos separada.

Variáveis opcionais no PC:
- PBDL_STREAM_WIDTH
- PBDL_STREAM_HEIGHT
- PBDL_STREAM_QUALITY
- PBDL_FRAME_PORT

Para máxima fluidez, mantenha os padrões 960x540 / qualidade 35.


## v7.8 Native JPEG

O cliente tenta decodificar os frames JPEG com libSceJpegDec do proprio PS4.
stb_image fica apenas como fallback se o decoder nativo nao estiver disponivel.

Preset MAX FPS padrao:
- 854x480
- JPEG qualidade 32
- stream TCP persistente 32125
- upscale para 1920x1080 via Piglet/GLES

Para priorizar nitidez no PC, execute start_windows_quality.bat:
- 960x540
- JPEG qualidade 38
