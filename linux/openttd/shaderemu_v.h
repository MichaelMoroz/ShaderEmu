/* OpenTTD's screen in a Nano-X window, drawn by the machine's GPU (docs/games/openttd.md). */

#ifndef VIDEO_SHADEREMU_H
#define VIDEO_SHADEREMU_H

#include "video_driver.hpp"

class VideoDriver_ShaderEmu : public VideoDriver {
public:
	std::optional<std::string_view> Start(const StringList &param) override;
	void Stop() override;
	void MakeDirty(int left, int top, int width, int height) override;
	void MainLoop() override;
	bool ChangeResolution(int w, int h) override;
	bool ToggleFullscreen(bool fullscreen) override;
	bool AfterBlitterChange() override;
	bool HasEfficient8Bpp() const override { return true; }
	std::string_view GetName() const override { return "shaderemu"; }
	static bool TickRun(uint32_t budget);

protected:
	Dimension GetScreenSize() const override;
	void InputLoop() override;
	bool LockVideoBuffer() override;
	void UnlockVideoBuffer() override;
	void Paint() override;
	void CheckPaletteAnim() override;
	bool PollEvent() override;

private:
	Palette local_palette{};
	Rect dirty_rect{};
	bool Resize(int w, int h);
	void DriverTick();
	void WorkerTick();
	static void TickEntry();
};

/* The game tick on a worker of its own (shaderemu_t.cpp; docs/games/openttd.md, "The tick on a worker") */
bool TwStart();
void TwStop();
bool TwActive();
bool TwHeld();
void TwHold(bool between); ///< the tick waits from now on (between: between two ticks), what it left made
void TwRelease();
bool TwLightPicture(); ///< the next picture may leave the tick running (docs/games/openttd.md, "Pictures the tick does not wait for")
bool TwMayGoLight(); ///< holding the tick: the rest of this picture may let it go
void TwSync();
void TwService();
void TwGrant();
void TwAfterFlush(); ///< the tick's frees made, once no flush is out
void TwPace(uint32_t work, uint32_t target, bool scrolling); ///< after a picture: the ticks spaced out while it scrolls late
void TwTimes(uint32_t &tick, uint32_t &park, uint32_t &instret);
extern uint32_t _tw_hold_time, _tw_holds, _tw_hold_most;
void TickAfter(); ///< at the end of each game tick (OPENTTD_SUM_AT)

class FVideoDriver_ShaderEmu : public DriverFactoryBase {
public:
	FVideoDriver_ShaderEmu() : DriverFactoryBase(Driver::DT_VIDEO, 10, "shaderemu", "ShaderEmu Nano-X Video Driver") {}
	std::unique_ptr<Driver> CreateInstance() const override { return std::make_unique<VideoDriver_ShaderEmu>(); }
};

#endif /* VIDEO_SHADEREMU_H */
