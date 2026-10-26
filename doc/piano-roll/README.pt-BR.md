# Piano roll para o Furnace

Fork do [Furnace](https://github.com/tildearrow/furnace) v0.6.8.3 com uma janela de piano roll por canal. O roll lê e escreve direto nas patterns do tracker. Não existe um formato novo de música: tudo o que você faz no roll vira notas, instrumentos, volumes e efeitos comuns, que o Furnace original abre normalmente.

![demonstração](piano-roll-demo.gif)

## Abrir

`window` > `piano roll`, ou **Ctrl+Shift+R** (dá para trocar o atalho em Settings > Keyboard).

## A janela, de cima para baixo

- **Barra, linha 1:** canal, ferramentas (Select **S**, Draw **D**, Erase **E**), Snap, fim das notas desenhadas (OFF ou REL), Fold, Ghost notes, Follow playback.
- **Barra, linha 2:** zoom, o que fazer com pattern repetida, Lanes, Clear transport e dicas ou avisos.
- **Painel de nota:** início, fim, duração, altura, instrumento e efeitos da nota selecionada (ou de várias).
- **Barra de arranjo:** um bloco por posição da order. Patterns repetidas aparecem em âmbar com "xN". Clicar num bloco rola até ele.
- **Régua:** linhas e compassos. Clicar põe o marcador de início.
- **Roll:** teclado à esquerda e notas do canal; os outros canais do mesmo chip aparecem em cinza (Ghost notes).
- **Faixas (Lanes):** volume e um efeito à escolha, linha a linha.

## Editar notas

| Ação | Como |
|---|---|
| Criar | Draw: clique (usa a última duração) ou clique e arraste |
| Mover | arraste o corpo da nota |
| Mudar a duração | arraste a borda direita |
| Duplicar | Shift + arrastar (a cópia vai para o lado da original, nunca em cima) |
| Selecionar várias | Select: retângulo em área vazia; Ctrl+clique soma ou tira |
| Apagar | Delete, ou ferramenta Erase (clique ou arraste por cima) |
| Selecionar tudo / limpar | Ctrl+A / Esc |
| Desfazer / refazer | Ctrl+Z / Ctrl+Y (o mesmo histórico do tracker) |

Cada gesto vira um único Ctrl+Z. Um canal do tracker é monofônico: uma nota começa onde a anterior acaba. Por isso o roll nunca apaga uma nota que não faz parte da edição; quem encurta é a nota editada.

Mover ou duplicar uma nota leva junto os efeitos da linha onde ela começa, o volume dessa linha e o corte `ECxx` dentro dela. Efeitos que valem para a música inteira (velocidade, tempo, pulos de order, fim de música) ficam no lugar.

**Pattern repetida:** se a edição cai numa pattern usada em mais de uma posição da order, o roll pergunta: editar todas, tornar única naquela posição ou cancelar. A resposta vale até fechar o Furnace e pode ser trocada na barra.

**Instrumento:** com notas selecionadas, clicar num instrumento da lista Instruments aplica esse instrumento a elas.

## Painel de nota

Mostra a nota selecionada, ou as várias selecionadas ("..." quando os valores diferem). Início e fim usam ordem:linha em hexadecimal, como o tracker. Duração, transposição (-12, -1, +1, +12) e instrumento valem para todas as selecionadas.

**Efeitos** aparecem agrupados pelo código. Editar o valor muda as linhas que já têm o efeito; **+N** acrescenta nas que não têm; **x** remove. **Add** escreve um efeito novo em todas: digite o código, Enter, o valor, Enter. Duplo clique numa nota leva direto ao campo Add. Se uma linha não tiver coluna de efeito livre, ela é pulada e a barra avisa. O roll não cria colunas sozinho; para isso, use o `++` no canto do tracker.

## Faixas de volume e de efeito

- **Volume:** degraus com o volume em vigor (ele vale até a próxima escrita). Arraste para desenhar; botão direito ou Erase apaga.
- **Efeito:** o seletor à esquerda escolhe o efeito; `*` significa "o mais usado do canal". A altura da barra é o valor (0 a FF). Clicar numa linha leva a linha ao painel, mesmo sem nota ali, e é assim que efeitos soltos como `EC04` são editados. Com Draw, arrastar escreve o efeito; botão direito ou Erase remove.

## Portamento

Uma nota com `03xx` ganha uma linha amarela que sai da nota anterior, sobe ou desce em rampa até a altura nova e segue pela nota. O tamanho da rampa é o tempo real do deslize, calculado como o Furnace faz em linear pitch (o padrão). Arraste a bolinha no fim da rampa para mudar o valor do `03`. Um círculo vazado avisa que a nota tem `03` mas nada soando antes, e portanto não desliza.

## Snap e precisão dentro da linha

- **row:** notas nas linhas.
- **1/2 e tick:** início e fim dentro da linha, escritos como `EDxx` (atraso) e `ECxx` (corte). Se não houver coluna de efeito livre para eles, a edição inteira é recusada e nada muda.

## Nomes de linha

Botão direito numa tecla do teclado do roll dá um nome àquela altura no canal ("kick", "snare", nome do sample). O nome aparece na tecla e nas notas.

## Sincronia com o tracker

Clicar numa nota leva o cursor do tracker até ela. Mover o cursor do tracker leva o roll para aquele canal e linha. Durante a reprodução, o roll não mexe no cursor do tracker.

## Transporte (botão "piano roll" ao lado de Poly)

Com o botão ligado:
- **Espaço** toca e para, em qualquer janela (por cima do atalho normal do Furnace), exceto enquanto você digita num campo.
- O play começa no **marcador** (clique na régua). Clicar na régua com a música tocando pula para lá.
- **I** e **O** marcam o início e o fim do loop no marcador. Com **Repeat pattern** ligado, a música toca em loop dentro da seleção, cortando as notas na volta.
- **Clear transport** limpa o marcador e o loop. F5 continua tocando do começo da música.

Com o botão desligado, o Furnace se comporta como o original.

## Arquivos

As preferências por canal (fim OFF/REL, Fold, nomes de linha) ficam num arquivo ao lado da música, `<música>.fur.roll.json`. O `.fur` não muda de formato. Zoom, canal, ferramenta, Snap, Ghost, Follow e Lanes ficam na configuração do Furnace.

## Idioma

Os textos da janela estão traduzidos para português do Brasil. Escolha o idioma em Settings > General > Language e reinicie o Furnace. A pasta `locale` precisa estar ao lado do `furnace.exe`.

## Limites conhecidos

- Edita um canal por vez.
- Marcador e loop funcionam por linha, não por tick, e valem até fechar o Furnace.
- Em músicas que não usam linear pitch, a rampa do portamento é aproximada (desenhada tracejada).
- O `.exe` não é assinado: o Windows mostra o aviso de fonte desconhecida.

## Código-fonte e licença

GPL-2.0 ou posterior, como o Furnace. O código-fonte completo desta versão vai junto do programa (`furnace-piano-roll-src.tar.gz`), e as mudanças sobre a v0.6.8.3 estão em `piano-roll.patch`. Instruções de compilação em `BUILD.md`.
