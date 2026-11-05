#include "ScribaARA.h"

#if JucePlugin_Enable_ARA

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<ScribaDocumentController>();
}

#endif
