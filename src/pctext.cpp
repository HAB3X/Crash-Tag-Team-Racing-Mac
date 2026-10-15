// The game's texts about the PlayStation 2 (its memory cards and their slots, the DUALSHOCK 2 and its ports, the console)
// said as a computer's, as the Wrath of Cortex and Crash Twinsanity ports do: each line's original is checked first, and each
// new line is written into the old line's own space (every one is shorter), so a different executable or data is left alone.
// Every original and its replacement is in PC_TEXT.md. Button names (START, X) are kept: a gamepad has them.
//
// Two places:
// - the executable's error screen (ErrorScreen, 0x1C3158: its message is the table at 0x511888, nine Latin-1 lines for each
//   of seven languages, the language's index from 0x242868): the controller's two lines, "disconnected" for either port and
//   for one port (the screen puts the port's number at its '$'; the new lines have none, and the screen's search then finds
//   nothing to replace). Checked and changed before the boot.
// - the text bibles (Scrooby's, in the archives' .p3d files: frontend, ingame, onfoot and loading; each language a chunk
//   0x1800E of UTF-16 lines found by a hash of their names). The bible's loader (0x370748) reads each language with
//   0x370508, overridden here: once a language is read, every line of it that is one of the originals below, in full, is
//   replaced. Only the lines of PS2 versions are in the table (named ps2_..., and six unnamed ones that say PlayStation 2,
//   its console or its MEMORY CARD slot): the bible also has the GameCube's, Xbox's and PSP's, which this game never shows.
//   The French, German and Spanish lines are older ones (they still say The Simpsons Hit & Run); the Dutch and Italian
//   bibles hold English or "???", and their English lines are matched as the English ones are.
//
// A language as 0x370508 makes it (36 bytes): +16 its letter, +20 the lines' count, +24 the hashes, +28 each line's offset
// in bytes into +32, the lines (UTF-16, each ended by a 0).
#include "ps2/iop.h"
#include "ps2/recomp.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

extern "C" void f_00370508(Ps2Cpu* c);

namespace
{
struct ErrorLine
{
    u32 slot; // the error screen table's pointer to the line
    const char* original;
    const char* text;
};

const ErrorLine ErrorLines[] = {
    // English, either port
    {0x005118a0,
     "The analog controller (DUALSHOCK\xae""2) is disconnected. Please reconnect the analog controller "
     "(DUALSHOCK\xae""2) to controller port 1 or controller port 2 and press START button to continue.",
     "The controller is disconnected. Please reconnect the controller and press START button to continue."},
    // English, its port ($)
    {0x005118a4,
     "The analog controller DUALSHOCK\xae""2 is disconnected. Please reconnect the analog controller "
     "DUALSHOCK\xae""2  to controller port $ and press START button to continue.",
     "The controller is disconnected. Please reconnect the controller and press START button to continue."},
    // French, either port
    {0x005118c4,
     "La manette analogique (DUALSHOCK\xae""2) est d\xe9""connect\xe9""e. Reconnecter la manette "
     "analogique (DUALSHOCK\xae""2) au port de manette N\xb0""1 ou au port de manette N\xb0""2 et appuyer "
     "sur la touche START pour continuer.",
     "La manette est d\xe9""connect\xe9""e. Reconnecter la manette et appuyer sur la touche START pour "
     "continuer."},
    // French, its port ($)
    {0x005118c8,
     "La manette analogique (DUALSHOCK\xae""2) est d\xe9""connect\xe9""e. Veuillez reconnecter la manette "
     "analogique (DUALSHOCK\xae""2) au port de manette N\xb0""$ et appuyer sur la touche START pour "
     "continuer.",
     "La manette est d\xe9""connect\xe9""e. Reconnecter la manette et appuyer sur la touche START pour "
     "continuer."},
    // Italian, either port
    {0x005118e8,
     "Il controller analogico (DUALSHOCK\xae""2) \xe8"" scollegato. Riconnettere il controller analogico "
     "(DUALSHOCK\xae""2) all'ingresso controller 1 o all'ingresso controller 2 e premere il tasto START "
     "(avvio) per continuare.",
     "Il controller \xe8"" scollegato. Riconnettere il controller e premere il tasto START (avvio) per "
     "continuare."},
    // Italian, its port ($)
    {0x005118ec,
     "Il controller analogico (DUALSHOCK\xae""2) \xe8"" scollegato. Riconnettere il controller analogico "
     "(DUALSHOCK\xae""2) all'ingresso controller $ e premere il tasto START (avvio) per continuare.",
     "Il controller \xe8"" scollegato. Riconnettere il controller e premere il tasto START (avvio) per "
     "continuare."},
    // German, either port
    {0x0051190c,
     "Der Analog Controller (DUALSHOCK\xae""2) wurde entfernt. Bitte den Analog Controller "
     "(DUALSHOCK\xae""2) wieder anschlie\xdf""en und START dr\xfc""cken, um fortzufahren.",
     "Der Controller wurde entfernt. Bitte den Controller wieder anschlie\xdf""en und START dr\xfc""cken, "
     "um fortzufahren."},
    // German, its port ($)
    {0x00511910,
     "Der Analog Controller (DUALSHOCK\xae""2) wurde entfernt. Bitte den Analog Controller "
     "(DUALSHOCK\xae""2) wieder an Controller-Anschluss $ anschlie\xdf""en und START dr\xfc""cken, um "
     "fortzufahren.",
     "Der Controller wurde entfernt. Bitte den Controller wieder anschlie\xdf""en und START dr\xfc""cken, "
     "um fortzufahren."},
    // Spanish, either port
    {0x00511930,
     "El mando anal\xf3""gico (DUALSHOCK\xae""2) est\xe1"" desconectado. Vuelve a conectar el mando "
     "anal\xf3""gico (DUALSHOCK\xae""2) y pulsa START para continuar.",
     "El mando est\xe1"" desconectado. Vuelve a conectar el mando y pulsa START para continuar."},
    // Spanish, its port ($)
    {0x00511934,
     "El mando anal\xf3""gico (DUALSHOCK\xae""2) est\xe1"" desconectado. Vuelve a conectar el mando "
     "anal\xf3""gico (DUALSHOCK\xae""2) al puerto de mando $ y pulsa START para continuar.",
     "El mando est\xe1"" desconectado. Vuelve a conectar el mando y pulsa START para continuar."},
    // Dutch, either port
    {0x00511954,
     "De analoge controller (DUALSHOCK\xae""2) is niet aangesloten. Sluit de analoge controller "
     "(DUALSHOCK\xae""2) opnieuw aan en druk op START om verder te gaan.",
     "De controller is niet aangesloten. Sluit de controller opnieuw aan en druk op START om verder te "
     "gaan."},
    // Dutch, its port ($)
    {0x00511958,
     "De analoge controller (DUALSHOCK\xae""2) is niet aangesloten. Sluit de analoge controller "
     "(DUALSHOCK\xae""2) opnieuw aan op controller-aansluiting $ en druk op START om verder te gaan.",
     "De controller is niet aangesloten. Sluit de controller opnieuw aan en druk op START om verder te "
     "gaan."},
    // English (the USA's wording), either port
    {0x00511978,
     "The DUALSHOCK\xae""2 analog controller is disconnected. Please reconnect the DUALSHOCK\xae""2 analog "
     "controller to controller port 1 or controller port 2 and press START button to continue.",
     "The controller is disconnected. Please reconnect the controller and press START button to continue."},
    // English (the USA's wording), its port ($)
    {0x0051197c,
     "The DUALSHOCK\xae""2 analog controller is disconnected. Please reconnect the DUALSHOCK\xae""2 analog "
     "controller to controller port $ and press START button to continue.",
     "The controller is disconnected. Please reconnect the controller and press START button to continue."},
};

struct BibleLine
{
    std::u16string_view original;
    std::u16string_view text;
};

constexpr BibleLine BibleLines[] = {
    // DEFGIS onfoot: 001bb60c
    {u"UNLOCKED\n'HARDLY EVER LAND'\nSTUNT ARENA.\nCONNECT YOUR PSP™ (PlayStation®Portable) TO YOUR "
     u"PlayStation®2 CONSOLE WITH A USB CABLE TO UNLOCK BONUS CONTENT.",
     u"UNLOCKED\n'HARDLY EVER LAND'\nSTUNT ARENA.\nCONNECT YOUR PSP™ (PlayStation®Portable) TO YOUR "
     u"COMPUTER WITH A USB CABLE TO UNLOCK BONUS CONTENT."},
    // E frontend: 001c9220
    {u"SELECT NUMBER OF PLAYERS ON THIS CONSOLE:",
     u"SELECT NUMBER OF LOCAL PLAYERS:"},
    // DEFGIS onfoot: 002137f6
    {u"CONNECT YOUR PSP™ (PlayStation®Portable) TO YOUR PlayStation®2 CONSOLE WITH A USB CABLE TO UNLOCK "
     u"BONUS CONTENT.",
     u"CONNECT YOUR PSP™ (PlayStation®Portable) TO YOUR COMPUTER WITH A USB CABLE TO UNLOCK BONUS CONTENT."},
    // E frontend: 0025670c
    {u"Connect To PlayStation®2",
     u"Connect To Computer"},
    // E frontend: 00299971
    {u"No memory card (8MB)(for\u00a0PlayStation®2) inserted in MEMORY CARD slot 1. If you wish to save "
     u"your game data, insert a memory card (8MB) (for PlayStation®2) with at least 99 KB of free space or "
     u"press the X button to continue without saving.",
     u"Save data is not available. If you wish to save your game data, free at least 99 KB of space on your "
     u"computer or press the X button to continue without saving."},
    // E frontend: 002c82d2
    {u"MEMORY CARD slot 1",
     u"SAVE DATA"},
    // E frontend: IHJ
    {u"Memory card (8MB)(for\u00a0PlayStation®2) is unusable.",
     u"The save data is unusable."},
    // E frontend: ps2_FileError_DataCorrupt_Loading
    {u"Load failed! Crash  Tag Team Racing Save Game file on memory card (8MB)(for\u00a0PlayStation®2) in "
     u"MEMORY CARD slot 1 is corrupt and must be deleted. OK?",
     u"Load failed! The Crash Tag Team Racing Save Game file is corrupt and must be deleted. OK?"},
    // E frontend: ps2_FileError_DataCorrupt_Saving
    {u"Save failed! Crash Tag Team Racing Save Game file on memory card (8MB)(for\u00a0PlayStation®2) in "
     u"MEMORY CARD slot 1 is corrupt and must be deleted. OK?",
     u"Save failed! The Crash Tag Team Racing Save Game file is corrupt and must be deleted. OK?"},
    // E frontend: ps2_FileError_FileNotFound, ps2_FileError_MediaNotFormatted_Loading
    {u"No Crash Tag Team Racing save data present on memory card (8MB)(for PlayStation®2) in MEMORY CARD "
     u"slot 1.",
     u"No Crash Tag Team Racing save data present."},
    // E frontend: ps2_FileError_MediaCorrupt
    {u"Insufficient free space on memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Crash "
     u"Tag Team Racing requires 99 KB of free space to save data.",
     u"Insufficient free space for save data. Crash Tag Team Racing requires 99 KB of free space to save "
     u"data."},
    // E frontend: ps2_FileError_MediaCorrupt_Checking
    {u"Insufficient free space on memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Insert a "
     u"memory card (8MB)(for\u00a0PlayStation®2) with at least 99 KB of free space, or create some space "
     u"using the console's internal browser.",
     u"Insufficient free space for save data. Crash Tag Team Racing needs at least 99 KB of free space on "
     u"your computer."},
    // E frontend: ps2_FileError_MediaCorrupt_Loading, ps2_FileError_MediaCorrupt_Saving
    {u"Save failed!  Please check memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 and try "
     u"again.",
     u"Save failed!  Please check the save data and try again."},
    // E frontend: ps2_FileError_MediaNotFormatted
    {u"Memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 is unformatted.",
     u"The save data is unformatted."},
    // E frontend: ps2_FileError_MediaNotFormatted_Saving
    {u"Save failed! memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 is unformatted.",
     u"Save failed! The save data is unformatted."},
    // E frontend: ps2_FileError_NoMedia_Loading, ps2_FileError_WrongMedia_Loading
    {u"Load failed! Check memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 and please try "
     u"again.",
     u"Load failed! Check the save data and please try again."},
    // E frontend: ps2_FileError_NoMedia_Saving, ps2_FileError_WrongMedia_Saving
    {u"Save failed! Check memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 and please try "
     u"again.",
     u"Save failed! Check the save data and please try again."},
    // E frontend: ps2_checking_memory_cards
    {u"Checking for a memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Do not remove memory "
     u"card (8MB)(for\u00a0PlayStation®2), controller, or reset/switch off the console.",
     u"Checking save data. Do not quit the game or turn off your computer."},
    // DEFGIS frontend: ps2_invalid_net_configs
    {u"The Your Network Configuration file on the memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD "
     u"slot 1 was created on a different console and cannot be used. Do you want to try again?",
     u"The Your Network Configuration file in the save data was created on a different computer and cannot "
     u"be used. Do you want to try again?"},
    // E frontend: ps2_msg_autoloading
    {u"Auto-loading most recent saved game. Do not remove memory card (8MB)(for\u00a0PlayStation®2) in "
     u"MEMORY CARD slot 1, DUALSHOCK®2 analog controller or reset/switch off the console.",
     u"Auto-loading most recent saved game. Do not quit the game or turn off your computer."},
    // E frontend: ps2_msg_autosave_off
    {u"The status of the memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 has changed. "
     u"Autosave has been disabled. To turn this feature back on, manually save from the Pause Menu or the "
     u"Main Menu.",
     u"The status of the save data has changed. Autosave has been disabled. To turn this feature back on, "
     u"manually save from the Pause Menu or the Main Menu."},
    // E frontend: ps2_msg_autosave_off_frontend
    {u"The status of the memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 has changed. "
     u"Autosave has been disabled. To turn this feature back on, manually save from the next screen.",
     u"The status of the save data has changed. Autosave has been disabled. To turn this feature back on, "
     u"manually save from the next screen."},
    // DEFGIS frontend: ps2_msg_autosave_warning
    {u"This game has an auto-save feature.  When you see the icon below, do not remove memory card "
     u"(8MB)(for\u00a0PlayStation®2), DUALSHOCK®2 analog controller,or reset/switch off the console.",
     u"This game has an auto-save feature.  When you see the icon below, do not quit the game or turn off "
     u"your computer."},
    // E frontend: ps2_msg_card_removed
    {u"The memory card (8MB)(for\u00a0PlayStation®2) has been removed from MEMORY CARD slot 1.",
     u"The save data is no longer available."},
    // E frontend: ps2_msg_format_confirm
    {u"Memory Card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 is unformatted. Do you want to "
     u"format it?",
     u"The save data is unformatted. Do you want to format it?"},
    // E frontend: ps2_msg_format_confirm2
    {u"Are you sure you wish to format memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1?",
     u"Are you sure you wish to format the save data?"},
    // E frontend: ps2_msg_format_failed
    {u"Format failed! Please check memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 and try "
     u"again.",
     u"Format failed! Please check the save data and try again."},
    // E frontend: ps2_msg_formatting
    {u"Formatting memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Do not remove memory "
     u"card (8MB)(for\u00a0PlayStation®2), DUALSHOCK®2 analog controller, or reset/switch off the console.",
     u"Formatting save data. Do not quit the game or turn off your computer."},
    // E frontend: ps2_msg_load_card_empty
    {u"No Crash Tag Team Racing save data present on memory card (8MB)(for\u00a0PlayStation®2) in MEMORY "
     u"CARD slot 1.",
     u"No Crash Tag Team Racing save data present."},
    // E frontend: ps2_msg_loading
    {u"Loading data. Do not remove memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1, "
     u"DUALSHOCK®2 analog controller or reset/switch off the console.",
     u"Loading data. Do not quit the game or turn off your computer."},
    // E frontend: ps2_msg_saving
    {u"Saving data.\nDo not remove memory card (8MB)(for\u00a0PlayStation®2), DUALSHOCK®2 analog "
     u"controller,or reset/switch off the console.",
     u"Saving data.\nDo not quit the game or turn off your computer."},
    // E frontend: ps2_mu_damaged
    {u"The memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 is damaged and cannot be used.",
     u"The save data is damaged and cannot be used."},
    // E frontend: ps2_mu_full
    {u"Insufficient space on the memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Crash Tag "
     u"Team Racing requires 99 KB of free space to save game.",
     u"Insufficient space for save data. Crash Tag Team Racing requires 99 KB of free space to save game."},
    // E frontend: ps2_mu_full_has_existing
    {u"Insufficient space on memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Crash Tag "
     u"Team Racing requires 99 KB of free space to save game. You can only overwrite existing save file.",
     u"Insufficient space for save data. Crash Tag Team Racing requires 99 KB of free space to save game. "
     u"You can only overwrite existing save file."},
    // E frontend: ps2_mu_full_no_existing
    {u"Insufficient space on memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1.Crash Tag Team "
     u"Racing requires 99 KB of free space to save game.",
     u"Insufficient space for save data. Crash Tag Team Racing requires 99 KB of free space to save game."},
    // E frontend: ps2_mu_full_no_existing_check
    {u"Insufficient free space on memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Insert a "
     u"memory card (8MB)(for\u00a0PlayStation®2) with at least 99 KB of free space into MEMORY CARD slot 1. "
     u"Do you wish to continue?",
     u"Insufficient free space for save data. At least 99 KB of free space is needed on your computer. Do "
     u"you wish to continue?"},
    // E frontend: ps2_mu_unformatted
    {u"Memory card (8MB)(for\u00a0PlayStation®2) is unformatted.",
     u"The save data is unformatted."},
    // E frontend: ps2_mu_wrongencoding
    {u"Memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1 is encoded for different market.",
     u"The save data is encoded for a different market."},
    // E frontend: ps2_no_memcard_net_config
    {u"No memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Please insert the memory card "
     u"(8MB)(for\u00a0PlayStation®2) storing Your Network Configuration file in MEMORY CARD slot 1.  Retry?",
     u"No Your Network Configuration file was found in the save data.  Retry?"},
    // E frontend: ps2_no_memory_cards
    {u"No memory card (8MB)(for\u00a0PlayStation®2) inserted in MEMORY CARD slot 1.",
     u"No save data is available."},
    // DEFGIS frontend: ps2_no_net_configs
    {u"A valid Your Network Configuration file was not found on the memory card "
     u"(8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Do you want to try again?",
     u"A valid Your Network Configuration file was not found in the save data. Do you want to try again?"},
    // E frontend: ps2_usbsave
    {u"Do not remove memory card (8MB)(for\u00a0PlayStation®2), DUALSHOCK®2 analog controller,or "
     u"reset/switch off the console.",
     u"Do not quit the game or turn off your computer."},
    // DFGIS frontend: ps2_msg_autosave_off, ps2_msg_autosave_off_frontend
    {u"The memory card (8MB)(for\u00a0PlayStation®2) has been removed from MEMORY CARD slot 1.\nAuto-save "
     u"has been disabled.",
     u"The save data is no longer available.\nAuto-save has been disabled."},
    // DFGIS frontend: ps2_no_memcard_net_config
    {u"No memory card (8MB)(for\u00a0PlayStation®2) in MEMORY CARD slot 1. Please insert the memory card "
     u"(8MB)(for\u00a0PlayStation®2) storing Your Network Configuration File in MEMORY CARD slot 1.  Retry?",
     u"No Your Network Configuration File was found in the save data.  Retry?"},
    // F frontend: 00299971
    {u"Pas de memory card (PS2) insérée dans la fente pour MEMORY CARD Nº$. Pour sauvegarder les données du "
     u"jeu, insérer une memory card (PS2) avec au moins 221 KB d'espace libre ou appuyer sur X pour "
     u"continuer sans sauvegarder.",
     u"Sauvegardes indisponibles. Pour sauvegarder les données du jeu, libérer au moins 221 KB d'espace sur "
     u"l'ordinateur ou appuyer sur X pour continuer sans sauvegarder."},
    // F frontend: 002c82d2
    {u"Fente pour MEMORY CARD Nº",
     u"SAUVEGARDES"},
    // F frontend: IHJ
    {u"La memory card (PS2) est inutilisable.",
     u"Les sauvegardes sont inutilisables."},
    // F frontend: ps2_FileError_MediaCorrupt
    {u"Espace insuffisant sur la memory card (PS2) dans la fente pour MEMORY CARD Nº$. The Simpsons Hit & "
     u"Run nécessite 221 KB d'espace libre pour sauvegarder les données.",
     u"Espace insuffisant pour les sauvegardes. 221 KB d'espace libre sont nécessaires pour sauvegarder les "
     u"données."},
    // F frontend: ps2_FileError_MediaCorrupt_Checking
    {u"Espace insuffisant sur la memory card (PS2) dans la fente pour MEMORY CARD Nº$. Insérer une memory "
     u"card (PS2) avec au moins 221 KB d'espace libre, ou libérer des fichiers à l'aide du navigateur "
     u"interne de la console.",
     u"Espace insuffisant pour les sauvegardes. Libérer au moins 221 KB d'espace sur l'ordinateur."},
    // F frontend: ps2_FileError_MediaCorrupt_Loading, ps2_FileError_WrongMedia_Loading
    {u"Echec du chargement ! Vérifier la memory card (PS2) dans la fente pour MEMORY CARD Nº$ et réessayer.",
     u"Echec du chargement ! Vérifier les sauvegardes et réessayer."},
    // F frontend: ps2_FileError_MediaCorrupt_Saving, ps2_FileError_WrongMedia_Saving
    {u"Echec de la sauvegarde ! Vérifier la memory card (PS2) dans la fente pour MEMORY CARD Nº$ et "
     u"réessayer.",
     u"Echec de la sauvegarde ! Vérifier les sauvegardes et réessayer."},
    // F frontend: ps2_FileError_MediaNotFormatted
    {u"La memory card (PS2) dans la fente pour MEMORY CARD Nº$ n'est pas formatée.",
     u"Les sauvegardes ne sont pas formatées."},
    // F frontend: ps2_FileError_MediaNotFormatted_Loading
    {u"Echec du chargement ! La memory card (PS2) dans la fente pour MEMORY CARD Nº$ n'est pas formatée.",
     u"Echec du chargement ! Les sauvegardes ne sont pas formatées."},
    // F frontend: ps2_FileError_MediaNotFormatted_Saving
    {u"Echec de la sauvegarde ! La memory card (PS2) dans la fente pour MEMORY CARD Nº$ n'est pas formatée.",
     u"Echec de la sauvegarde ! Les sauvegardes ne sont pas formatées."},
    // F frontend: ps2_FileError_NoMedia_Loading
    {u"Échec du chargement ! Vérifier la memory card (PS2) dans la fente pour MEMORY CARD Nº$ et réessayer.",
     u"Échec du chargement ! Vérifier les sauvegardes et réessayer."},
    // F frontend: ps2_FileError_NoMedia_Saving
    {u"Échec de la sauvegarde ! Vérifier la memory card (PS2) dans la fente pour MEMORY CARD Nº$ et "
     u"réessayer.",
     u"Échec de la sauvegarde ! Vérifier les sauvegardes et réessayer."},
    // F frontend: ps2_checking_memory_cards
    {u"Vérification de la memory card (PS2) dans la fente pour MEMORY CARD Nº1 et la fente pour MEMORY CARD "
     u"Nº2. Ne pas retirer la memory card (PS2), ni débrancher de manette ou relancer/éteindre la console.",
     u"Vérification des sauvegardes. Ne pas quitter le jeu ni éteindre l'ordinateur."},
    // F frontend: ps2_msg_autoloading
    {u"Chargement automatique de la sauvegarde la plus récente en cours. Ne pas retirer la memory card "
     u"(PS2) dans la fente pour MEMORY CARD Nº$, ni retirer de manette, redémarrer ou éteindre la console.",
     u"Chargement automatique de la sauvegarde la plus récente en cours. Ne pas quitter le jeu ni éteindre "
     u"l'ordinateur."},
    // F frontend: ps2_msg_card_removed, ps2_msg_loading
    {u"Chargement en cours. Ne pas retirer la memory card (PS2) dans la fente pour MEMORY CARD Nº$, ni "
     u"retirer de manette, redémarrer ou éteindre la console.",
     u"Chargement en cours. Ne pas quitter le jeu ni éteindre l'ordinateur."},
    // F frontend: ps2_msg_format_confirm
    {u"La memory card (PS2) dans la fente pour MEMORY CARD Nº$ n'est pas formatée. Formater la memory card "
     u"(PS2) ?",
     u"Les sauvegardes ne sont pas formatées. Formater les sauvegardes ?"},
    // F frontend: ps2_msg_format_confirm2
    {u"Formater la memory card (PS2) dans la fente pour MEMORY CARD Nº$ ?",
     u"Formater les sauvegardes ?"},
    // F frontend: ps2_msg_format_failed
    {u"Échec du formatage ! Vérifier la memory card (PS2) dans la fente pour MEMORY CARD Nº$ et réessayer.",
     u"Échec du formatage ! Vérifier les sauvegardes et réessayer."},
    // F frontend: ps2_msg_formatting
    {u"Formatage de la memory card (PS2) dans la fente pour MEMORY CARD Nº$. Ne pas retirer la memory card "
     u"(PS2) ou la manette, ni redémarrer ou éteindre la console.",
     u"Formatage des sauvegardes. Ne pas quitter le jeu ni éteindre l'ordinateur."},
    // F frontend: ps2_msg_load_card_empty
    {u"Pas de données Simpsons Hit & Run sur la memory card (PS2) dans la fente pour MEMORY CARD Nº$.",
     u"Aucune donnée sauvegardée."},
    // F frontend: ps2_msg_saving
    {u"Sauvegarde en cours. Ne pas retirer la memory card (PS2) ou la manette, ni redémarrer ou éteindre la "
     u"console.",
     u"Sauvegarde en cours. Ne pas quitter le jeu ni éteindre l'ordinateur."},
    // F frontend: ps2_mu_damaged
    {u"Cette memory card (PS2) est endommagée et ne peut pas être utilisée.",
     u"Les sauvegardes sont endommagées et ne peuvent pas être utilisées."},
    // F frontend: ps2_mu_full, ps2_mu_full_no_existing, ps2_mu_full_no_existing_check
    {u"Espace insuffisant sur la memory card (PS2). The Simpsons Hit & Run nécessite 221 KB d'espace libre "
     u"pour sauvegarder.",
     u"Espace insuffisant pour les sauvegardes. 221 KB d'espace libre sont nécessaires pour sauvegarder."},
    // F frontend: ps2_mu_full_has_existing
    {u"Espace insuffisant sur la memory card (PS2). The Simpsons Hit & Run nécessite 221 KB pour "
     u"sauvegarder. Vous ne pouvez qu'écraser la sauvegarde.",
     u"Espace insuffisant pour les sauvegardes. 221 KB sont nécessaires pour sauvegarder. Vous ne pouvez "
     u"qu'écraser la sauvegarde."},
    // F frontend: ps2_mu_unformatted
    {u"La memory card (PS2) n'est pas formatée",
     u"Les sauvegardes ne sont pas formatées"},
    // F frontend: ps2_mu_wrongencoding
    {u"La memory card (PS2) est prévue pour un autre marché.",
     u"Les sauvegardes sont prévues pour un autre marché."},
    // F frontend: ps2_no_memory_cards
    {u"Pas de memory card (PS2) dans la fente pour MEMORY CARD Nº1 ou la fente pour MEMORY CARD Nº2.",
     u"Aucune sauvegarde disponible."},
    // G frontend: 00299971
    {u"Keine Memory Card (PS2) in MEMORY CARD-Steckplatz $ eingelegt. Zum Speichern der Spieldaten leg "
     u"bitte eine Memory Card (PS2) mit mindestens 221 KB freiem Speicherplatz ein oder drück X, um ohne zu "
     u"speichern fortzufahren.",
     u"Speichern nicht möglich. Zum Speichern der Spieldaten schaff bitte mindestens 221 KB freien "
     u"Speicherplatz auf dem Computer oder drück X, um ohne zu speichern fortzufahren."},
    // G frontend: 002c82d2
    {u"MEMORY CARD-Steckplatz ",
     u"SPIELSTÄNDE"},
    // G frontend: IHJ
    {u"Die Memory Card (PS2) ist unbrauchbar.",
     u"Die Spielstände sind unbrauchbar."},
    // G frontend: ps2_FileError_MediaCorrupt
    {u"Ungenügend Speicherplatz auf der Memory Card (PS2) in MEMORY CARD-Steckplatz $. The Simpsons Hit & "
     u"Run benötigt 221 KB freien Speicherplatz, um Daten zu speichern.",
     u"Ungenügend Speicherplatz für Spielstände. Es werden 221 KB freier Speicherplatz benötigt, um Daten "
     u"zu speichern."},
    // G frontend: ps2_FileError_MediaCorrupt_Checking
    {u"Ungenügend Speicherplatz auf der Memory Card (PS2) in MEMORY CARD-Steckplatz $. Bitte leg eine "
     u"Memory Card (PS2) mit mindestens 221 KB freiem Speicherplatz ein oder schaff mithilfe des internen "
     u"Browsers der Konsole Platz.",
     u"Ungenügend Speicherplatz für Spielstände. Bitte schaff mindestens 221 KB freien Speicherplatz auf "
     u"dem Computer."},
    // G frontend: ps2_FileError_MediaCorrupt_Loading, ps2_FileError_MediaCorrupt_Saving, ps2_FileError_NoMedia_Saving,
    // ps2_FileError_WrongMedia_Saving
    {u"Speichern gescheitert! Bitte überprüf die Memory Card (PS2) in MEMORY CARD-Steckplatz $ und versuch "
     u"es noch einmal.",
     u"Speichern gescheitert! Bitte überprüf die Spielstände und versuch es noch einmal."},
    // G frontend: ps2_FileError_MediaNotFormatted
    {u"Die Memory Card (PS2) in MEMORY CARD-Steckplatz $ ist nicht formatiert.",
     u"Die Spielstände sind nicht formatiert."},
    // G frontend: ps2_FileError_MediaNotFormatted_Loading
    {u"Laden gescheitert! Die Memory Card (PS2) in MEMORY CARD-Steckplatz $ ist nicht formatiert.",
     u"Laden gescheitert! Die Spielstände sind nicht formatiert."},
    // G frontend: ps2_FileError_MediaNotFormatted_Saving
    {u"Speichern gescheitert! Die Memory Card (PS2) in MEMORY CARD-Steckplatz $ ist nicht formatiert.",
     u"Speichern gescheitert! Die Spielstände sind nicht formatiert."},
    // G frontend: ps2_FileError_NoMedia_Loading, ps2_FileError_WrongMedia_Loading
    {u"Laden gescheitert! Bitte überprüf die Memory Card (PS2) in MEMORY CARD-Steckplatz $ und versuch es "
     u"noch einmal.",
     u"Laden gescheitert! Bitte überprüf die Spielstände und versuch es noch einmal."},
    // G frontend: ps2_checking_memory_cards
    {u"Memory Card (PS2) in MEMORY CARD-Steckplatz 1 und 2 wird überprüft. Memory card (PS2) und Controller "
     u"nicht entfernen, und die Konsole nicht zurückstellen oder abschalten.",
     u"Spielstände werden überprüft. Das Spiel nicht beenden und den Computer nicht ausschalten."},
    // G frontend: ps2_msg_autoloading
    {u"Zuletzt gespeichertes Spiel wird automatisch geladen. Memory Card (PS2) in MEMORY CARD-Steckplatz $ "
     u"und Controller nicht entfernen und die Konsole nicht zurückstellen oder abschalten.",
     u"Zuletzt gespeichertes Spiel wird automatisch geladen. Das Spiel nicht beenden und den Computer nicht "
     u"ausschalten."},
    // G frontend: ps2_msg_card_removed, ps2_msg_loading
    {u"Ladevorgang. Memory Card (PS2) in MEMORY CARD-Steckplatz $ und Controller nicht entfernen und die "
     u"Konsole nicht zurückstellen oder abschalten.",
     u"Ladevorgang. Das Spiel nicht beenden und den Computer nicht ausschalten."},
    // G frontend: ps2_msg_format_confirm
    {u"Die Memory Card (PS2) in MEMORY CARD-Steckplatz $ ist nicht formatiert. Memory Card (PS2) "
     u"formatieren?",
     u"Die Spielstände sind nicht formatiert. Spielstände formatieren?"},
    // G frontend: ps2_msg_format_confirm2
    {u"Bist du sicher, dass du die Memory Card (PS2) in MEMORY CARDSteckplatz $ formatieren möchtest?",
     u"Bist du sicher, dass du die Spielstände formatieren möchtest?"},
    // G frontend: ps2_msg_format_failed
    {u"Formatieren gescheitert! Bitte überprüf die Memory Card (PS2) in MEMORY CARD-Steckplatz $ und "
     u"versuch es noch einmal.",
     u"Formatieren gescheitert! Bitte überprüf die Spielstände und versuch es noch einmal."},
    // G frontend: ps2_msg_formatting
    {u"Formatiert Memory Card (PS2) in MEMORY CARD-Steckplatz $. memory card (PS2) und Controller nicht "
     u"entfernen und die Konsole nicht zurückstellen oder abschalten.",
     u"Spielstände werden formatiert. Das Spiel nicht beenden und den Computer nicht ausschalten."},
    // G frontend: ps2_msg_load_card_empty
    {u"Keine gespeicherten Daten von THE SIMPSONS HIT & RUN auf der Memory Card (PS2) in MEMORY "
     u"CARD-Steckplatz $ vorhanden.",
     u"Keine gespeicherten Daten vorhanden."},
    // G frontend: ps2_msg_saving
    {u"Speichert Daten. Memory Card (PS2) und Controller nicht entfernen und die Konsole nicht "
     u"zurückstellen oder abschalten.",
     u"Speichert Daten. Das Spiel nicht beenden und den Computer nicht ausschalten."},
    // G frontend: ps2_mu_damaged
    {u"Diese Memory Card (PS2) ist beschädigt und kann nicht verwendet werden.",
     u"Die Spielstände sind beschädigt und können nicht verwendet werden."},
    // G frontend: ps2_mu_full
    {u"Ungenügend Platz auf der Memory Card (PS2). Leg eine Memory Card (PS2) mit mindestens 221 KB freiem "
     u"Speicherplatz ein oder schaff Platz mit Hilfe des internen Browsers der Konsole.",
     u"Ungenügend Platz für Spielstände. Bitte schaff mindestens 221 KB freien Speicherplatz auf dem "
     u"Computer."},
    // G frontend: ps2_mu_full_has_existing
    {u"Ungenügend Speicherplatz auf der Memory Card (PS2). The Simpsons Hit & Run benötigt 221 KB freien "
     u"Speicherplatz, um Daten zu speichern. Eine bestehende Datei muss überschrieben werden.",
     u"Ungenügend Speicherplatz für Spielstände. Es werden 221 KB freier Speicherplatz benötigt, um Daten "
     u"zu speichern. Eine bestehende Datei muss überschrieben werden."},
    // G frontend: ps2_mu_full_no_existing, ps2_mu_full_no_existing_check
    {u"Ungenügend Speicherplatz auf der Memory Card (PS2). The Simpsons Hit & Run benötigt 221 KB freien "
     u"Speicherplatz, um Daten zu speichern.",
     u"Ungenügend Speicherplatz für Spielstände. Es werden 221 KB freier Speicherplatz benötigt, um Daten "
     u"zu speichern."},
    // G frontend: ps2_mu_unformatted
    {u"Die Memory Card (PS2) ist nicht formatiert.",
     u"Die Spielstände sind nicht formatiert."},
    // G frontend: ps2_mu_wrongencoding
    {u"Memory Card (PS2) ist für andere Region kodiert.",
     u"Die Spielstände sind für andere Region kodiert."},
    // G frontend: ps2_no_memory_cards
    {u"Keine Memory Card (PS2) in MEMORY CARD-Steckplatz 1 oder MEMORY CARD-Steckplatz 2.",
     u"Keine Spielstände verfügbar."},
    // S frontend: 00299971
    {u"No hay memory card (PS2) en la ranura para MEMORY CARD $. Si deseas guardar datos del juego, inserta "
     u"una memory card (PS2) con al menos 221 KB de espacio libre o pulsa X para continuar sin guardar.",
     u"No se puede guardar. Si deseas guardar datos del juego, libera al menos 221 KB de espacio en el "
     u"ordenador o pulsa X para continuar sin guardar."},
    // S frontend: 002c82d2
    {u"ranura para MEMORY CARD ",
     u"datos guardados"},
    // S frontend: IHJ
    {u"No se puede usar la memory card (PS2)",
     u"No se pueden usar los datos guardados"},
    // S frontend: ps2_FileError_MediaCorrupt
    {u"No hay espacio suficiente libre en la memory card (PS2) de la ranura para MEMORY CARD $. The "
     u"Simpsons Hit & Run necesita 221 KB de espacio libre para guardar datos.",
     u"No hay espacio suficiente libre para los datos guardados. Se necesitan 221 KB de espacio libre para "
     u"guardar datos."},
    // S frontend: ps2_FileError_MediaCorrupt_Checking
    {u"No hay espacio suficiente libre en la memory card (PS2) de la ranura para MEMORY CARD $. Inserta una "
     u"memory card (PS2) con al menos 221 KB de espacio libre, o crea espacio libre con el navegador "
     u"interno de la consola.",
     u"No hay espacio suficiente libre para los datos guardados. Libera al menos 221 KB de espacio en el "
     u"ordenador."},
    // S frontend: ps2_FileError_MediaCorrupt_Loading, ps2_FileError_MediaCorrupt_Saving, ps2_FileError_NoMedia_Saving,
    // ps2_FileError_WrongMedia_Saving
    {u"¡Error al guardar! Comprueba la memory card (PS2) de la ranura para MEMORY CARD $ e inténtalo de "
     u"nuevo.",
     u"¡Error al guardar! Comprueba los datos guardados e inténtalo de nuevo."},
    // S frontend: ps2_FileError_MediaNotFormatted
    {u"La memory card (PS2) de la ranura para MEMORY CARD $ no tiene formato.",
     u"Los datos guardados no tienen formato."},
    // S frontend: ps2_FileError_MediaNotFormatted_Loading
    {u"¡Error al cargar! La memory card (PS2) de la ranura para MEMORY CARD $ no tiene formato.",
     u"¡Error al cargar! Los datos guardados no tienen formato."},
    // S frontend: ps2_FileError_MediaNotFormatted_Saving
    {u"¡Error al guardar! La memory card (PS2) de la ranura para MEMORY CARD $ no tiene formato.",
     u"¡Error al guardar! Los datos guardados no tienen formato."},
    // S frontend: ps2_FileError_NoMedia_Loading, ps2_FileError_WrongMedia_Loading
    {u"¡Error al cargar! Comprueba la memory card (PS2) de la ranura para MEMORY CARD $ e inténtalo de "
     u"nuevo.",
     u"¡Error al cargar! Comprueba los datos guardados e inténtalo de nuevo."},
    // S frontend: ps2_checking_memory_cards
    {u"Comprobando la memory card (PS2) de la ranura para MEMORY CARD 1 y la ranura para MEMORY CARD 2. No "
     u"extraigas la memory card (PS2) ni el mando, y no reinicies o apagues la consola.",
     u"Comprobando los datos guardados. No salgas del juego ni apagues el ordenador."},
    // S frontend: ps2_msg_autoloading
    {u"Autocargando la partida guardada más reciente. No extraigas la memory card (PS2) de la ranura para "
     u"MEMORY CARD $ ni el mando. No reinicies ni apagues la consola.",
     u"Autocargando la partida guardada más reciente. No salgas del juego ni apagues el ordenador."},
    // S frontend: ps2_msg_card_removed, ps2_msg_loading
    {u"Cargando datos. No extraigas la memory card (PS2) de la ranura para MEMORY CARD $ ni el mando. No "
     u"reinicies ni apagues la consola.",
     u"Cargando datos. No salgas del juego ni apagues el ordenador."},
    // S frontend: ps2_msg_format_confirm
    {u"La memory card (PS2) de la ranura para MEMORY CARD $ no tiene formato. ¿Deseas formatear la memory "
     u"card (PS2)?",
     u"Los datos guardados no tienen formato. ¿Deseas formatear los datos guardados?"},
    // S frontend: ps2_msg_format_confirm2
    {u"¿Seguro que quieres formatear la memory card (PS2) de la ranura para MEMORY CARD $?",
     u"¿Seguro que quieres formatear los datos guardados?"},
    // S frontend: ps2_msg_format_failed
    {u"¡Error al formatear! Comprueba la memory card (PS2) en la ranura para MEMORY CARD $ e inténtalo de "
     u"nuevo.",
     u"¡Error al formatear! Comprueba los datos guardados e inténtalo de nuevo."},
    // S frontend: ps2_msg_formatting
    {u"Formateando la memory card (PS2) de la ranura para MEMORY CARD $. No extraigas la memory card (PS2) "
     u"ni el mando. No reinicies ni apagues la consola.",
     u"Formateando los datos guardados. No salgas del juego ni apagues el ordenador."},
    // S frontend: ps2_msg_load_card_empty
    {u"No hay datos guardados de Simpsons Hit & Run en la memory card (PS2) de la ranura para MEMORY CARD "
     u"$.",
     u"No hay datos guardados."},
    // S frontend: ps2_msg_saving
    {u"Guardando datos. No extraigas la memory card (PS2) ni el mando. No reinicies ni apagues la consola.",
     u"Guardando datos. No salgas del juego ni apagues el ordenador."},
    // S frontend: ps2_mu_damaged
    {u"Esta memory card (PS2) está dañada y no se puede utilizar.",
     u"Los datos guardados están dañados y no se pueden utilizar."},
    // S frontend: ps2_mu_full, ps2_mu_full_no_existing, ps2_mu_full_no_existing_check
    {u"No hay espacio suficiente en la memory card (PS2). The Simpsons Hit & Run necesita 221 KB de espacio "
     u"libre para guardar la partida.",
     u"No hay espacio suficiente para los datos guardados. Se necesitan 221 KB de espacio libre para "
     u"guardar la partida."},
    // S frontend: ps2_mu_full_has_existing
    {u"No hay espacio suficiente en la memory card (PS2). The Simpsons Hit & Run necesita 221 KB de espacio "
     u"libre para guardar la partida. Sólo se puede sobrescribir el archivo guardado.",
     u"No hay espacio suficiente para los datos guardados. Se necesitan 221 KB de espacio libre para "
     u"guardar la partida. Sólo se puede sobrescribir el archivo guardado."},
    // S frontend: ps2_mu_unformatted
    {u"La memory card (PS2) no tiene formato",
     u"Los datos guardados no tienen formato"},
    // S frontend: ps2_mu_wrongencoding
    {u"La memory card (PS2) está codificada para otro mercado",
     u"Los datos guardados son de otro mercado"},
    // S frontend: ps2_no_memory_cards
    {u"No hay memory card (PS2) en la ranura para MEMORY CARD 1 o en la ranura para MEMORY CARD 2.",
     u"No hay datos guardados disponibles."},
};

// Every new line fits in its original's space (the new line and its 0 over the old line's bytes)
constexpr bool Fits()
{
    for (const BibleLine& line : BibleLines)
    {
        if (line.text.size() > line.original.size())
        {
            return false;
        }
    }
    return true;
}
static_assert(Fits(), "a new line is longer than its original");

const std::unordered_map<std::u16string_view, std::u16string_view>& BibleTable()
{
    static const std::unordered_map<std::u16string_view, std::u16string_view> table = [] {
        std::unordered_map<std::u16string_view, std::u16string_view> t;
        for (const BibleLine& line : BibleLines)
        {
            t.emplace(line.original, line.text);
        }
        return t;
    }();
    return table;
}

u32 Read32(u32 address)
{
    u32 v;
    std::memcpy(&v, Ps2::EeMemory(address), 4);
    return v;
}

// A UTF-16 line in the game's memory, up to its 0; empty when it's longer than limit characters
std::u16string ReadLine(u32 address, size_t limit)
{
    std::u16string line;
    for (size_t n = 0; n < limit; n++)
    {
        char16_t ch;
        std::memcpy(&ch, Ps2::EeMemory(address + 2 * n), 2);
        if (ch == 0)
        {
            return line;
        }
        line.push_back(ch);
    }
    return {};
}
}

void CttrPcText()
{
    for (const ErrorLine& line : ErrorLines)
    {
        if (std::strcmp(reinterpret_cast<const char*>(Ps2::EeMemory(Read32(line.slot))), line.original) != 0)
        {
            Ps2::Log("PC text: the error screen's line at %08x isn't the one known: its lines are left as they are", line.slot);
            return;
        }
    }
    for (const ErrorLine& line : ErrorLines)
    {
        std::memcpy(Ps2::EeMemory(Read32(line.slot)), line.text, std::strlen(line.text) + 1);
    }
}

// The bible's language reader (0x370508): the language read, then its lines about the console replaced
extern "C" void CttrReadLanguage(Ps2Cpu* c)
{
    f_00370508(c);
    u32 language = c->r[2].uw[0];
    if (language == 0)
    {
        return; // not the language in use: not kept
    }
    u32 count = Read32(language + 20);
    u32 offsets = Read32(language + 28);
    u32 lines = Read32(language + 32);
    size_t longest = 0;
    for (const BibleLine& line : BibleLines)
    {
        longest = std::max(longest, line.original.size());
    }
    int replaced = 0;
    for (u32 n = 0; n < count; n++)
    {
        u32 at = lines + (Read32(offsets + 4 * n) & ~1u);
        // An original's whole line and its 0, so a longer line that starts the same isn't taken for it
        std::u16string text = ReadLine(at, longest + 1);
        auto found = BibleTable().find(text);
        if (found == BibleTable().end())
        {
            continue;
        }
        std::u16string_view now = found->second;
        std::memcpy(Ps2::EeMemory(at), now.data(), 2 * now.size());
        std::memset(Ps2::EeMemory(at + 2 * static_cast<u32>(now.size())), 0, 2);
        replaced++;
    }
    if (replaced > 0)
    {
        Ps2::Log("PC text: %d lines of a text bible's language '%c' said as a computer's", replaced,
                 static_cast<char>(*Ps2::EeMemory(language + 16)));
    }
}
