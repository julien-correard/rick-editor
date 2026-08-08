#include <cstdlib>
#include <algorithm>

#include <SDL2/SDL.h>

extern "C" {
#include "tiles.h"   // tile_t, tiles_data[][] -- fournies par dat_tilesST.c
}
#include "mapdata.h" // ScaleFactor, FRow/FCol/DisBank, RED/GREEN/BLUE, map_bnums, map_blocks

// Nombre d'elements de map_bnums (0x1FD8), calcule plutot que recopie en dur.
static const int MAP_BNUMS_COUNT = sizeof(map_bnums) / sizeof(map_bnums[0]);

// drawmap() balaie une fenetre fixe de VIEW_COLS colonnes x VIEW_ROWS lignes
// de blocs (voir plus bas). On en deduit la valeur maximale de FRow pour
// laquelle tous les indices utilises restent dans map_bnums.
static const int VIEW_COLS = 8;
static const int VIEW_ROWS = 128; // 1024 cellules / 8 colonnes
static const int FROW_MAX = (MAP_BNUMS_COUNT - VIEW_COLS * VIEW_ROWS) / VIEW_COLS;

void drawtile (int x, int y, int NbBank, int NbTile, SDL_Surface* ecran)
{
    SDL_Surface *rectangle = NULL;
    SDL_Rect position;
    unsigned long tile[8];




    // Allocation de la surface

    rectangle = SDL_CreateRGBSurface(0, ScaleFactor, ScaleFactor, 32, 0, 0, 0, 0);

    // CORRECTIF : on lit desormais la banque passee en parametre (NbBank)
    // au lieu de relire la globale DisBank ici. Avant, ce parametre etait
    // recu mais jamais utilise, ce qui rendait le "1" code en dur dans
    // drawmap() totalement inoffensif par pur hasard -- la selection de
    // banque ne fonctionnait que parce que DisBank etait relue directement.
    // Desormais drawmap() transmet DisBank comme NbBank (voir plus bas) :
    // meme resultat visible, mais un seul chemin de donnees, coherent.
    for (int i=0;i<8;i++) //getting the tile from tiles_data
{
    tile[i]=  tiles_data[NbBank][NbTile][i];
}


int tiletab[64];
    int z=0,i=0;

while (z<64)
{
    tiletab[z++]=0;
}
z=0;

for (i=0;i!=8;i++)
{
while (tile[i] != 0)
    {
      tiletab[(7-z++)+(i*8)] = tile[i] % 0x10;
      tile[i] /= 0x10;
    }

z=0;
}



for (i = 0; i < 64; i = i + 1)

{

   position.x = ((i%8) * ScaleFactor)+(x*ScaleFactor); // Les coordonnees de la surface seront (0, 0)

    position.y = (int(i/8) * ScaleFactor)+(y*ScaleFactor);

    // Remplissage de la surface avec du blanc


     SDL_FillRect(rectangle, NULL, SDL_MapRGB(ecran->format, RED[tiletab[i]], GREEN[tiletab[i]], BLUE[tiletab[i]]));

    SDL_BlitSurface(rectangle, NULL, ecran, &position); // Collage de la surface sur l'ecran



}

SDL_FreeSurface(rectangle); // CORRECTIF : la surface temporaire n'etait jamais liberee (fuite memoire a chaque tuile dessinee -- des milliers de fois par frame).

}




void drawblock(int x, int y, int NbBank, int NbBlock,SDL_Surface*ecran)
{
for (int i=0;i<16;i++)
{
    drawtile(((i%4)*8)+x*32,(int(i/4)*8)+y*32,NbBank, map_blocks[NbBlock][i],ecran);//x,y,bank,tile,ecran
}
}

// NOTE PORTAGE : SDL2 separe la fenetre (SDL_Window) de sa surface
// (SDL_Surface) ; drawmap() a donc besoin des deux (la surface pour
// dessiner, la fenetre pour l'afficher via SDL_UpdateWindowSurface,
// equivalent SDL2 de l'ancien SDL_Flip(ecran)).
void drawmap (SDL_Window* fenetre, SDL_Surface* ecran)
{
    for (int i=0;i<VIEW_COLS*VIEW_ROWS;i++)
{
   // CORRECTIF : NbBank = DisBank (transmis reellement) au lieu du "1"
   // code en dur -- voir la note dans drawtile() ci-dessus.
   drawblock((i%VIEW_COLS)+FCol,int(i/VIEW_COLS),DisBank,map_bnums[i+FRow*VIEW_COLS],ecran);
}
SDL_UpdateWindowSurface(fenetre);
}

void leftclick(int MouseX, int MouseY, SDL_Window* fenetre, SDL_Surface* ecran)
{
    int X = (MouseX/ScaleFactor)/32;
    int Y = (MouseY/ScaleFactor)/32;

    // CORRECTIF : l'ancienne formule ((X%8)-FCol)*(Y+1) ne correspondait pas
    // a l'indexation reellement utilisee par drawmap() (map_bnums[i+FRow*8]),
    // ignorait FRow, et pouvait produire un indice negatif ou hors bornes
    // (ecriture memoire hors tableau). On reconstruit ici le meme indice que
    // drawmap() pour la case cliquee : colonne = X-FCol, ligne = Y+FRow.
    int col = X - FCol;
    int row = Y;
    if (col < 0 || col >= VIEW_COLS || row < 0 || row >= VIEW_ROWS)
        return; // clic hors de la zone de carte affichee : on ignore

    int PosBlock = col + (row + FRow) * VIEW_COLS;
    if (PosBlock < 0 || PosBlock >= MAP_BNUMS_COUNT)
        return; // securite supplementaire, ne devrait pas arriver

    // CORRECTIF : la valeur pouvait depasser 255 (dernier indice valide de
    // map_blocks[0x100]) et provoquer une lecture hors bornes au prochain
    // rendu. On boucle desormais sur les 256 blocs disponibles.
    map_bnums[PosBlock] = (map_bnums[PosBlock] + 1) % 256;

    drawmap(fenetre, ecran);
}

void pause(SDL_Window* fenetre, SDL_Surface* ecran)
{
    int continuer = 1;
    int MouseX, MouseY;
    SDL_Event event;

    while (continuer)
    {
        SDL_WaitEvent(&event);
        switch(event.type)
        {
            case SDL_QUIT:
                continuer = 0;
                break;
         case SDL_KEYDOWN:
             switch(event.key.keysym.sym)
                {
                    case SDLK_ESCAPE:
                        continuer = 0;
                        break;
                        case SDLK_UP:
                        FRow-=1;
                        FRow = std::clamp(FRow, 0, FROW_MAX); // CORRECTIF : bornage (evite un indice hors tableau dans drawmap)
                        drawmap(fenetre, ecran);
                        break;
                        case SDLK_DOWN:
                        FRow+=1;
                        FRow = std::clamp(FRow, 0, FROW_MAX); // CORRECTIF
                        drawmap(fenetre, ecran);
                        break;
                        case SDLK_LEFT:
                        FRow-=16;
                        FRow = std::clamp(FRow, 0, FROW_MAX); // CORRECTIF
                        drawmap(fenetre, ecran);
                        break;
                        case SDLK_RIGHT:
                        FRow+=16;
                        FRow = std::clamp(FRow, 0, FROW_MAX); // CORRECTIF
                        drawmap(fenetre, ecran);
                        break;
                        case SDLK_1:
                        DisBank = 1;
                        drawmap(fenetre, ecran);
                        break;
                        case SDLK_2:
                        DisBank=2;
                        drawmap(fenetre, ecran);
                        break;

                        }
                break;
        case SDL_MOUSEBUTTONDOWN:
            {
                // CORRECTIF : dans l'original, l'absence d'accolades autour
                // du if faisait que SEUL SDL_GetMouseState() etait vraiment
                // conditionne par "bouton gauche" -- leftclick() et le break
                // s'executaient pour N'IMPORTE QUEL bouton, avec des
                // coordonnees MouseX/MouseY potentiellement obsoletes (voire
                // non initialisees si aucun clic gauche n'avait encore eu
                // lieu). Le commentaire d'origine ("If the left button was
                // pressed") decrit bien l'intention ; on la restaure.
                if (event.button.button == SDL_BUTTON_LEFT)
                {
                    SDL_GetMouseState(&MouseX, &MouseY);
                    leftclick(MouseX,MouseY,fenetre,ecran);
                }
                break;
            }
    }

}

}



int main(int argc, char *argv[])

{
    (void)argc;
    (void)argv;

    SDL_Init(SDL_INIT_VIDEO);

    SDL_Window *fenetre = SDL_CreateWindow(
        "Ma super fenêtre SDL !", // CORRECTIF : reencode en UTF-8 (le fichier d'origine etait en ISO-8859-1, illisible sous Ubuntu)
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1000, 700, SDL_WINDOW_SHOWN);

    SDL_Surface *ecran = SDL_GetWindowSurface(fenetre);

    SDL_FillRect(ecran, NULL, SDL_MapRGB(ecran->format, 0, 0, 0));

drawmap(fenetre, ecran);
    pause(fenetre, ecran);

    SDL_DestroyWindow(fenetre); // CORRECTIF : liberation de la fenetre (absente de l'original, sans consequence sous Windows a la fermeture du process mais correcte a ajouter)

    SDL_Quit();


    return EXIT_SUCCESS;

}


