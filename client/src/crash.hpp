/*
 *  Voice Bridge client
 *
 *  Writes the location of a crash (module + offset, stack) to
 *  voicebridge.log before SA-MP's crash window appears, so crashes can be
 *  traced even when they are not in this mod.
 */

#pragma once

namespace vbc::crash
{
void Install();
}
