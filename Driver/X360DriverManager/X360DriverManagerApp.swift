//
//  X360DriverManagerApp.swift
//  X360DriverManager
//
//  Host app for the X360Driver DriverKit extension: activates it and shows status.
//

import SwiftUI

@main
struct X360DriverManagerApp: App {
    var body: some Scene {
        WindowGroup("Xbox 360 Driver") {
            ContentView()
        }
        .windowResizability(.contentSize)
    }
}
