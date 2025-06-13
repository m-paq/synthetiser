export interface IMyAPI {
	sendMessage: (message: string) => void
	endKeyboardGrab: (callback: () => void) => void,
	selectDeviceName: (callback: (args: string[]) => void) => void,
	grabKeyboard: () => void,
	selectMidiDevice: () => void
}

declare global {
	interface Window {
		electronAPI: IMyAPI
	}
}
