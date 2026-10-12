/* DD2 sound-effect table. The segment is set at game start:
   0x16AE: mov word [0x9342], 0x256D   -> file offset 0x256D0.
   Header: "SND\0" + uint16 (total length = 6248).
   Then 16-byte records: { uint16 seq_ptr; uint16 class; char name[8]; }.
   The sequence data lies at address 0x256D0 + seq_ptr.
   Generated from the image -- do not edit. */
#define DD2_SND_BASE 0x256D0
#define DD2_SND_LEN  6248
static const struct { const char *name; unsigned short off, cls; } dd2_sounds[] = {
  { "SHOTWALL", 0x0190, 0x0880 },
  { "SLIMESLO", 0x0218, 0x08c8 },
  { "JUMP    ", 0x0302, 0x0880 },
  { "LAND    ", 0x033e, 0x08c8 },
  { "SHOTMONS", 0x035c, 0x08c8 },
  { "SHOTMISS", 0x03fa, 0x08c8 },
  { "NOAMMO  ", 0x04ca, 0x08dc },
  { "KNIFETHR", 0x04ea, 0x08e1 },
  { "GAMEOVER", 0x0514, 0x08ff },
  { "DEATHSOU", 0x0782, 0x08ff },
  { "LEVELDON", 0x087c, 0x08ff },
  { "OPENDOOR", 0x0aec, 0x08c8 },
  { "RELOAD  ", 0x0c0c, 0x08c8 },
  { "GHOSTFAD", 0x0c1a, 0x08c8 },
  { "GHOSTREA", 0x0d54, 0x08c8 },
  { "TREASURE", 0x0e82, 0x08c8 },
  { "TREASURE", 0x0ed6, 0x08c8 },
  { "TREASURE", 0x0f56, 0x08c8 },
  { "TREASURE", 0x100c, 0x08c8 },
  { "TREASURE", 0x1126, 0x08c8 },
  { "EXTRADAV", 0x12a6, 0x08fa },
  { "LEVELSTA", 0x1516, 0x08ff },
  { "MASTFIRE", 0x1784, 0x08c8 },
  { "HITHEAD ", 0x1864, 0x08c8 },
};
#define DD2_NSOUNDS (sizeof dd2_sounds / sizeof dd2_sounds[0])
