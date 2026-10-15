// SPDX-License-Identifier: Apache-2.0
package org.oldgoatstudio.midgard;

import org.libsdl.app.SDLActivity;

/** The game's activity: SDL's, with SDL linked statically into the runtime (libmain.so). */
public class MidgardActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }
}
