import { ChangeDetectionStrategy, Component, Input } from '@angular/core';
import { NemesisBar } from '../../services/nemeses.service';

/**
 * Fallout 4 nemeses: the bar over a surrendered man the player holds at gunpoint (and on top of his questioning
 * card): his name and gang, how scared he is, and what the player sees on him.
 */
@Component({
  selector: 'app-nemesis-bar',
  templateUrl: './nemesis-bar.component.html',
  styleUrls: ['./nemesis-bar.component.scss'],
  changeDetection: ChangeDetectionStrategy.OnPush,
})
export class NemesisBarComponent {
  @Input() bar!: NemesisBar;
}
