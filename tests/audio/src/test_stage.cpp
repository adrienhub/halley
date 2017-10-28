#include "test_stage.h"
#include <array>

using namespace Halley;

void TestStage::init()
{
	getAudioAPI().playMusic(getResource<AudioClip>("Loveshadow_-_Marcos_Theme.ogg"));
}

void TestStage::onVariableUpdate(Time time)
{
	auto key = getInputAPI().getKeyboard();
	if (key->isButtonDown(Keys::Esc)) {
		getCoreAPI().quit();
	}

	if (key->isButtonPressed(Keys::M)) {
		playingMusic = !playingMusic;
		getAudioAPI().getMusic(0)->setGain(playingMusic ? 1 : 0);
	}

	auto noteKeys = std::array<int, 12>{{ Keys::A, Keys::W, Keys::S, Keys::E, Keys::D, Keys::F, Keys::T, Keys::G, Keys::Y, Keys::H, Keys::U, Keys::J }};
	auto noteSamples = std::array<String, 12>{{ "c1", "c1s", "d1", "d1s", "e1", "f1", "f1s", "g1", "g1s", "a1", "a1s", "b1" }};
	for (int i = 0; i < 12; ++i) {
		if (key->isButtonPressed(noteKeys[i])) {
			getAudioAPI().playUI(getResources().get<AudioClip>(noteSamples[i] + ".ogg"), 1.0f, -1.0f);
		}
	}
}

void TestStage::onRender(RenderContext& context) const
{
}
