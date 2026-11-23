#pragma once
/*
 * Texts from the box (Home Assistant values, nightly update results, error
 * messages for the setup page): English by default, German with
 * text_language = de in teddybox.ini (settings.text_de).
 * L("English", "Deutsch") picks one at run time; both may be printf formats
 * with the same arguments.
 */
#include "settings.h"

#define L(en, de) (settings_get()->text_de ? (de) : (en))
