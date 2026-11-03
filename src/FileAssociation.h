#pragma once

// Makes .frvm open with this executable (current user only, no admin rights needed). With
// makeDefault false the build only adds itself to Explorer's "Open with" list (CHARONOS Lite
// next to the full build). Returns true if anything had to be changed.
bool registerFrvmAssociation(bool makeDefault = true);
