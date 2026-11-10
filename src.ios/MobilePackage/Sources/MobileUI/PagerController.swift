// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The viewer's swipe and pinch: UIKit, because this is the one place SwiftUI cannot hold smooth
// paging and zoom (docs/plans/mobile.md M7). A page keeps the shape of the item, not of whichever
// picture has arrived, so a sharper one replaces the last with no layout shift.
import MobileKit
import QuartzCore
import SwiftUI
import UIKit

/// A jump the pager acts on once, however often SwiftUI redraws around it.
struct PagerJump: Equatable {
  let id = UUID()
  let index: Int
}

enum PageState: Equatable {
  case working
  case refining(doneBytes: Int64?, totalBytes: Int64?)
  case finished
  case failed(MediaFailure)
}

struct PagerView: UIViewControllerRepresentable {
  let entries: GridSource
  let startIndex: Int
  let images: ImageLadder
  let onIndex: (Int) -> Void
  let onTap: () -> Void
  let onZoomed: (Bool) -> Void
  let onPlay: (GridEntry) -> Void
  let onState: (MediaID, PageState) -> Void
  let onPullChanged: (Bool) -> Void
  let onClose: () -> Void
  let onSettled: (Int) -> Void
  /// A request to show a page, from the filmstrip.
  let jump: PagerJump?
  /// Something opaque is over the pages, so VoiceOver must not reach them.
  let isCovered: Bool

  func makeUIViewController(context: Context) -> PagerController {
    PagerController(entries: entries, startIndex: startIndex, images: images, onIndex: onIndex,
                    onTap: onTap, onZoomed: onZoomed, onPlay: onPlay, onState: onState,
                    onPullChanged: onPullChanged, onClose: onClose, onSettled: onSettled)
  }

  func updateUIViewController(_ controller: PagerController, context: Context) {
    if let jump { controller.go(to: jump) }
    controller.view.accessibilityElementsHidden = isCovered
  }
}

final class PagerController: UIViewController, UICollectionViewDataSource, UICollectionViewDelegateFlowLayout,
  UIGestureRecognizerDelegate {
  private let entries: GridSource
  private let images: ImageLadder
  private let onIndex: (Int) -> Void
  private let onTap: () -> Void
  private let onZoomed: (Bool) -> Void
  private let onPlay: (GridEntry) -> Void
  private let onState: (MediaID, PageState) -> Void
  private let onPullChanged: (Bool) -> Void
  private let onClose: () -> Void
  private let onSettled: (Int) -> Void

  private var index: Int
  private var laidOutSize = CGSize.zero
  /// Each page's latest state, told to SwiftUI only once the swipe is over.
  private var states: [MediaID: PageState] = [:]
  private var isMoving: Bool { collection.isDragging || collection.isDecelerating }
  private static let pageGap: CGFloat = 20

  private let layout = UICollectionViewFlowLayout()
  private lazy var collection = UICollectionView(frame: .zero, collectionViewLayout: layout)

  init(entries: GridSource, startIndex: Int, images: ImageLadder, onIndex: @escaping (Int) -> Void,
       onTap: @escaping () -> Void, onZoomed: @escaping (Bool) -> Void,
       onPlay: @escaping (GridEntry) -> Void, onState: @escaping (MediaID, PageState) -> Void,
       onPullChanged: @escaping (Bool) -> Void, onClose: @escaping () -> Void,
       onSettled: @escaping (Int) -> Void) {
    self.entries = entries
    index = startIndex
    self.images = images
    self.onIndex = onIndex
    self.onTap = onTap
    self.onZoomed = onZoomed
    self.onPlay = onPlay
    self.onState = onState
    self.onPullChanged = onPullChanged
    self.onClose = onClose
    self.onSettled = onSettled
    super.init(nibName: nil, bundle: nil)
  }

  @available(*, unavailable) required init?(coder: NSCoder) { fatalError() }

  override func viewDidLoad() {
    super.viewDidLoad()
    view.backgroundColor = .black
    layout.scrollDirection = .horizontal
    layout.minimumLineSpacing = 0
    layout.minimumInteritemSpacing = 0
    collection.backgroundColor = .black
    collection.isPagingEnabled = true
    collection.showsHorizontalScrollIndicator = false
    collection.contentInsetAdjustmentBehavior = .never
    collection.dataSource = self
    collection.delegate = self
    collection.register(ZoomPageCell.self, forCellWithReuseIdentifier: "page")
    view.addSubview(collection)
    let pull = UIPanGestureRecognizer(target: self, action: #selector(pulled(_:)))
    pull.delegate = self
    collection.addGestureRecognizer(pull)
  }

  // Pulling a photo down closes it. Only straight down, and never while it is zoomed in.
  func gestureRecognizerShouldBegin(_ gesture: UIGestureRecognizer) -> Bool {
    guard let pan = gesture as? UIPanGestureRecognizer, pan.view === collection else { return true }
    let v = pan.velocity(in: view)
    let zoomed = collection.visibleCells.contains { ($0 as? ZoomPageCell)?.isZoomed == true }
    return v.y > 0 && v.y > abs(v.x) && !zoomed
  }

  @objc private func pulled(_ pan: UIPanGestureRecognizer) {
    let y = max(0, pan.translation(in: view).y)
    switch pan.state {
    case .began:
      onPullChanged(true)
    case .changed:
      let scale = max(0.82, 1 - y / 1600)
      collection.transform = CGAffineTransform(translationX: 0, y: y).scaledBy(x: scale, y: scale)
    case .ended where y > 110 || pan.velocity(in: view).y > 900:
      onClose()
    default:
      let reset = { self.collection.transform = .identity }
      if UIAccessibility.isReduceMotionEnabled {
        reset()
      } else {
        UIView.animate(withDuration: 0.25, delay: 0, usingSpringWithDamping: 1, initialSpringVelocity: 0,
                       options: [.allowUserInteraction], animations: reset)
      }
      onPullChanged(false)
    }
  }

  override func viewDidLayoutSubviews() {
    super.viewDidLayoutSubviews()
    guard view.bounds.size != laidOutSize, view.bounds.width > 0 else { return }
    laidOutSize = view.bounds.size
    // The collection is one gap wider than the screen, so pages are apart while swiping.
    collection.frame = CGRect(x: 0, y: 0, width: view.bounds.width + Self.pageGap, height: view.bounds.height)
    layout.itemSize = collection.bounds.size
    layout.invalidateLayout()
    collection.layoutIfNeeded()
    collection.contentOffset = CGPoint(x: CGFloat(index) * collection.bounds.width, y: 0)
    prefetchAround(index)
  }

  private func prefetchAround(_ center: Int) {
    let near = (center - 2...center + 2).filter { $0 != center && entries.indices.contains($0) }
    images.prefetch(near.map { entries[$0].item }, fittingPixels: fittingPixels)
  }

  private var fittingPixels: CGSize {
    let scale = view.window?.screen.scale ?? 3
    return CGSize(width: view.bounds.width * scale, height: view.bounds.height * scale)
  }

  func collectionView(_ collectionView: UICollectionView, numberOfItemsInSection section: Int) -> Int { entries.count }

  func collectionView(_ collectionView: UICollectionView, cellForItemAt indexPath: IndexPath) -> UICollectionViewCell {
    let cell = collectionView.dequeueReusableCell(withReuseIdentifier: "page", for: indexPath) as! ZoomPageCell
    let entry = entries[indexPath.item]
    cell.show(entry: entry, images: images, trailingGap: Self.pageGap, fittingPixels: fittingPixels,
              onTap: onTap, onPlay: { [onPlay] in onPlay(entry) },
              onZoomed: { [weak self] zoomed in
                guard let self else { return }
                if indexPath.item == self.index { self.onZoomed(zoomed) }
              },
              onState: { [weak self] id, state in self?.record(id, state) },
              holdsRefinement: { [weak self] in self?.isMoving ?? false })
    return cell
  }

  func scrollViewDidScroll(_ scrollView: UIScrollView) {
    guard scrollView === collection, collection.bounds.width > 0, scrollView.isDragging || scrollView.isDecelerating
    else { return }
    let page = Int((scrollView.contentOffset.x / collection.bounds.width).rounded())
    guard page != index, entries.indices.contains(page) else { return }
    index = page
    prefetchAround(page)
  }

  private var lastJump: UUID?

  func go(to jump: PagerJump) {
    guard jump.id != lastJump, entries.indices.contains(jump.index), collection.bounds.width > 0 else { return }
    lastJump = jump.id
    let distance = abs(jump.index - index)
    index = jump.index
    prefetchAround(index)
    let offset = CGPoint(x: CGFloat(index) * collection.bounds.width, y: 0)
    // A neighbour slides in; anything further cuts, as flying past hundreds of photos would be noise.
    if distance == 1, !UIAccessibility.isReduceMotionEnabled {
      collection.setContentOffset(offset, animated: true)
    } else {
      collection.setContentOffset(offset, animated: false)
      collection.layoutIfNeeded()
      // This runs inside a SwiftUI update, which cannot take new state; the next turn can.
      DispatchQueue.main.async { [weak self] in self?.settled() }
    }
  }

  func scrollViewDidEndScrollingAnimation(_ scrollView: UIScrollView) {
    if scrollView === collection { settled() }
  }

  /// A swipe does nothing on the main thread but move pages. Everything SwiftUI needs to redraw,
  /// and each picture too big to upload inside a frame, waits for this.
  private func settled() {
    onIndex(index)
    // A bounce on the same page leaves its zoom alone; only the page on screen speaks for it.
    onZoomed((collection.cellForItem(at: IndexPath(item: index, section: 0)) as? ZoomPageCell)?.isZoomed ?? false)
    onSettled(index)
    let id = entries[index].item.id
    if let state = states[id] { onState(id, state) }
    for case let cell as ZoomPageCell in collection.visibleCells {
      if collection.indexPath(for: cell)?.item != index { cell.resetZoom() }
      cell.applyHeldRefinement()
    }
  }

  private func record(_ id: MediaID, _ state: PageState) {
    states[id] = state
    guard !isMoving, id == entries[index].item.id else { return }
    onState(id, state)
  }

  func scrollViewDidEndDecelerating(_ scrollView: UIScrollView) {
    // Work that does not belong to the swipe waits for it to finish.
    if scrollView === collection { settled() }
  }

  func scrollViewDidEndDragging(_ scrollView: UIScrollView, willDecelerate decelerate: Bool) {
    if scrollView === collection, !decelerate { settled() }
  }
}

final class ZoomPageCell: UICollectionViewCell, UIScrollViewDelegate {
  private let scroll = UIScrollView()
  private let imageView = UIImageView()
  private let playButton = UIButton(type: .system)

  private var entry: GridEntry?
  private var loading: Task<Void, Never>?
  private var trailingGap: CGFloat = 0
  private var onTap: (() -> Void)?
  private var onPlay: (() -> Void)?
  private var onZoomed: ((Bool) -> Void)?
  private var lastSize = CGSize.zero
  private var holdsRefinement: (() -> Bool)?
  private var heldRefinement: UIImage?

  override init(frame: CGRect) {
    super.init(frame: frame)
    contentView.backgroundColor = .black
    scroll.delegate = self
    scroll.showsVerticalScrollIndicator = false
    scroll.showsHorizontalScrollIndicator = false
    scroll.contentInsetAdjustmentBehavior = .never
    scroll.decelerationRate = .fast
    scroll.minimumZoomScale = 1
    imageView.contentMode = .scaleAspectFit
    imageView.isAccessibilityElement = true
    imageView.accessibilityTraits = .image
    scroll.addSubview(imageView)
    contentView.addSubview(scroll)

    let double = UITapGestureRecognizer(target: self, action: #selector(doubleTapped(_:)))
    double.numberOfTapsRequired = 2
    scroll.addGestureRecognizer(double)
    let single = UITapGestureRecognizer(target: self, action: #selector(singleTapped))
    single.require(toFail: double)
    scroll.addGestureRecognizer(single)

    var button = UIButton.Configuration.plain()
    button.image = UIImage(systemName: "play.fill", withConfiguration: UIImage.SymbolConfiguration(pointSize: 26, weight: .semibold))
    button.baseForegroundColor = .white
    button.background.backgroundColor = UIColor.black.withAlphaComponent(0.5)
    button.background.cornerRadius = 36
    playButton.configuration = button
    playButton.accessibilityLabel = "Play"
    playButton.addTarget(self, action: #selector(playTapped), for: .touchUpInside)
    contentView.addSubview(playButton)
  }

  @available(*, unavailable) required init?(coder: NSCoder) { fatalError() }

  override func prepareForReuse() {
    super.prepareForReuse()
    loading?.cancel()
    loading = nil
    entry = nil
    heldRefinement = nil
    imageView.image = nil
    resetZoom()
  }

  func resetZoom() { scroll.setZoomScale(1, animated: false) }

  var isZoomed: Bool { scroll.zoomScale > 1.01 }

  func show(entry: GridEntry, images: ImageLadder, trailingGap: CGFloat, fittingPixels: CGSize,
            onTap: @escaping () -> Void, onPlay: @escaping () -> Void,
            onZoomed: @escaping (Bool) -> Void, onState: @escaping (MediaID, PageState) -> Void,
            holdsRefinement: @escaping () -> Bool) {
    self.holdsRefinement = holdsRefinement
    self.entry = entry
    self.trailingGap = trailingGap
    self.onTap = onTap
    self.onPlay = onPlay
    self.onZoomed = onZoomed
    let item = entry.item
    imageView.accessibilityLabel = entry.accessibilityName
    playButton.isHidden = item.kind != .clip
    scroll.pinchGestureRecognizer?.isEnabled = item.kind == .photo
    // Rule 3: the first frame is whatever the grid already had.
    imageView.image = images.cachedImage(for: item.id)
    scroll.maximumZoomScale = 4
    lastSize = .zero
    setNeedsLayout()

    guard item.kind == .photo else {
      loading = Task { [weak self] in
        guard let poster = await images.thumbnail(for: item), !Task.isCancelled else { return }
        self?.imageView.image = poster
      }
      return
    }

    let started = CACurrentMediaTime()
    loading = Task { [weak self] in
      onState(item.id, .working)
      // Progress shows only past a second (§10), even when the source reports nothing.
      let late = Task {
        try? await Task.sleep(for: .seconds(1))
        if !Task.isCancelled { onState(item.id, .refining(doneBytes: nil, totalBytes: nil)) }
      }
      defer { late.cancel() }
      do {
        for try await event in images.ladder(for: item, fittingPixels: fittingPixels) {
          guard !Task.isCancelled else { return }
          switch event {
          case .image(let image, let isOriginal):
            if isOriginal, self?.holdsRefinement?() == true {
              self?.heldRefinement = image
            } else {
              self?.sharpen(to: image)
            }
          case .loading(let done, let total):
            if CACurrentMediaTime() - started >= 1 {
              late.cancel()
              onState(item.id, .refining(doneBytes: done, totalBytes: total))
            }
          }
        }
        guard !Task.isCancelled else { return }
        late.cancel()
        onState(item.id, .finished)
      } catch {
        guard !Task.isCancelled else { return }
        late.cancel()
        onState(item.id, .failed(error as? MediaFailure ?? .synologyUnreachable))
      }
    }
  }

  func applyHeldRefinement() {
    guard let image = heldRefinement else { return }
    heldRefinement = nil
    sharpen(to: image)
  }

  private func sharpen(to image: UIImage) {
    guard imageView.image != nil, !UIAccessibility.isReduceMotionEnabled else {
      imageView.image = image
      return
    }
    UIView.transition(with: imageView, duration: 0.2, options: [.transitionCrossDissolve, .allowUserInteraction]) {
      self.imageView.image = image
    }
  }

  override func layoutSubviews() {
    super.layoutSubviews()
    let page = CGRect(x: 0, y: 0, width: bounds.width - trailingGap, height: bounds.height)
    scroll.frame = page
    let side: CGFloat = 72
    playButton.frame = CGRect(x: page.midX - side / 2, y: page.midY - side / 2, width: side, height: side)
    guard page.size != lastSize, let entry, page.width > 0 else { return }
    lastSize = page.size
    scroll.zoomScale = 1
    let aspect = CGFloat(entry.item.aspect)
    let fit: CGSize = page.width / page.height > aspect
      ? CGSize(width: page.height * aspect, height: page.height)
      : CGSize(width: page.width, height: page.width / aspect)
    imageView.frame = CGRect(origin: .zero, size: fit)
    scroll.contentSize = fit
    centerImage()
  }

  private func centerImage() {
    let dx = max(0, (scroll.bounds.width - imageView.frame.width) / 2)
    let dy = max(0, (scroll.bounds.height - imageView.frame.height) / 2)
    scroll.contentInset = UIEdgeInsets(top: dy, left: dx, bottom: dy, right: dx)
  }

  func viewForZooming(in scrollView: UIScrollView) -> UIView? { entry?.item.kind == .photo ? imageView : nil }

  func scrollViewDidZoom(_ scrollView: UIScrollView) {
    centerImage()
    onZoomed?(scrollView.zoomScale > 1.01)
  }

  @objc private func singleTapped() { onTap?() }

  @objc private func playTapped() { onPlay?() }

  @objc private func doubleTapped(_ gesture: UITapGestureRecognizer) {
    guard entry?.item.kind == .photo else { return }
    let animated = !UIAccessibility.isReduceMotionEnabled
    if scroll.zoomScale > 1.01 {
      scroll.setZoomScale(1, animated: animated)
      return
    }
    let scale: CGFloat = 2.5
    let point = gesture.location(in: imageView)
    let size = CGSize(width: scroll.bounds.width / scale, height: scroll.bounds.height / scale)
    scroll.zoom(to: CGRect(x: point.x - size.width / 2, y: point.y - size.height / 2,
                           width: size.width, height: size.height), animated: animated)
  }
}
