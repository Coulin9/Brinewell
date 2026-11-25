import BrinewellCore
import SwiftUI

struct SetupView: View {
    @ObservedObject var model: AppModel
    @ObservedObject var diskDeployment: DiskDeployment
    @ObservedObject var d3dMetalDeployment: D3DMetalDeployment
    let readiness: SetupReadiness
    @Binding var currentStep: SetupStep
    let onDismiss: () -> Void
    @State private var showsProgress: Bool

    init(model: AppModel, diskDeployment: DiskDeployment, d3dMetalDeployment: D3DMetalDeployment,
         readiness: SetupReadiness, currentStep: Binding<SetupStep>,
         onDismiss: @escaping () -> Void) {
        self.model = model
        self.diskDeployment = diskDeployment
        self.d3dMetalDeployment = d3dMetalDeployment
        self.readiness = readiness
        self._currentStep = currentStep
        self.onDismiss = onDismiss
        self._showsProgress = State(initialValue: readiness.neededSteps.count > 1)
    }

    var body: some View {
        VStack(spacing: 0) {
            if showsProgress {
                HStack(spacing: 20) {
                    stepLabel("SteamOS System Disk", number: 1, step: .systemDisk,
                              completed: readiness.disk.isReady)
                    Divider().frame(height: 20)
                    stepLabel("Graphics Setup", number: 2, step: .graphics,
                              completed: readiness.graphics.isReady)
                }
                .padding(16)
                Divider()
            }
            switch currentStep {
            case .systemDisk:
                DiskDeploymentView(model: model, deployment: diskDeployment)
            case .graphics:
                GraphicsSetupView(d3dMetalDeployment: d3dMetalDeployment, readiness: readiness,
                                  onDismiss: onDismiss, onRecheck: { model.recheckRosetta() })
            }
        }
        .background(.regularMaterial)
    }

    private func stepLabel(_ title: LocalizedStringKey, number: Int, step: SetupStep, completed: Bool) -> some View {
        HStack(spacing: 6) {
            if completed {
                Image(systemName: "checkmark.circle.fill").accessibilityHidden(true)
            } else {
                Text("\(number)").monospacedDigit()
            }
            Text(title)
        }
        .font(.callout.weight(currentStep == step ? .semibold : .regular))
        .foregroundStyle(currentStep == step ? Color.accentColor : Color.secondary)
    }
}
