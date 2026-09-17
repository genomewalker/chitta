package reads
import scala.math.abs
class Reads
class Counts extends Reads {
 def total(x: Int): Int = normalize(x)
 def normalize(x: Int): Int = abs(x)
}
object Main { def run(): Int = new Counts().total(1) }
